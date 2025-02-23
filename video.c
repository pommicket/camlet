#include "video.h"

#include <stdatomic.h>
#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include "util.h"
#include "camera.h"

struct VideoContext {
	double start_time;
	AVFormatContext *avf_context;
	AVCodecContext *video_encoder;
	AVFrame *video_frame;
	AVPacket *av_packet;
	AVStream *video_stream;
	int64_t next_video_pts;
	SDL_Thread *audio_thread;
	bool recording;
	char _unused0[128]; // reduce false sharing
	SDL_mutex *audio_mutex;
	SDL_AudioStream *audio_stream;
	bool audio_quit;
	char _unused1[128]; // reduce false sharing
};

static int audio_thread(void *data) {
	VideoContext *ctx = data;
	if (!ctx->audio_mutex) return -1;
	pa_sample_spec audio_format = {
		.format = PA_SAMPLE_S16LE,
		.rate = 44100,
		.channels = 2,
	};
	int err = 0;
	pa_simple *pulseaudio = pa_simple_new(NULL, "camlet", PA_STREAM_RECORD, NULL,
		"microphone", &audio_format, NULL, NULL, &err);
	if (!pulseaudio) {
		fprintf(stderr, "%s", pa_strerror(err));
	}
	bool warned = false;
	bool quit = false;
	while (!quit) {
		char buf[2048];
		if (pa_simple_read(pulseaudio, buf, sizeof buf, &err) < 0) {
			if (!warned) {
				fprintf(stderr, "pa_simple_read: %s", pa_strerror(err));
				warned = true;
			}
			SDL_LockMutex(ctx->audio_mutex);
			quit = ctx->audio_quit;
			SDL_UnlockMutex(ctx->audio_mutex);
		} else {
			SDL_LockMutex(ctx->audio_mutex);
			quit = ctx->audio_quit;
			if (!quit && ctx->audio_stream)
				SDL_AudioStreamPut(ctx->audio_stream, buf, sizeof buf);
			SDL_UnlockMutex(ctx->audio_mutex);
		}
	}
	pa_simple_free(pulseaudio);
	return 0;
}


VideoContext *video_init(void) {
	VideoContext *ctx = calloc(1, sizeof(VideoContext));
	if (!ctx) return NULL;
	ctx->audio_mutex = SDL_CreateMutex();
	if (ctx->audio_mutex) {
		ctx->audio_thread = SDL_CreateThread(audio_thread, "camlet audio thread", ctx);
	}
	return ctx;
}

bool video_start(VideoContext *ctx, const char *filename, int32_t width, int32_t height, int fps, int quality) {
	if (!ctx) return false;
	if (ctx->recording) {
		return true;
	}
	video_stop(ctx);
	int err = avformat_alloc_output_context2(&ctx->avf_context, NULL, NULL, filename);
	if (!ctx->avf_context) {
		fprintf(stderr, "error: avformat_alloc_output_context2: %s\n", av_err2str(err));
		return false;
	}
	const AVOutputFormat *fmt = ctx->avf_context->oformat;
	const AVCodec *video_codec = avcodec_find_encoder(fmt->video_codec);
	if (!video_codec) {
		fprintf(stderr, "couldn't find encoder for codec %s\n", avcodec_get_name(fmt->video_codec));
		return false;
	}
	ctx->video_stream = avformat_new_stream(ctx->avf_context, NULL);
	ctx->video_stream->id = 0;
	ctx->video_encoder = avcodec_alloc_context3(video_codec);
	if (!ctx->video_encoder) {
		fprintf(stderr, "couldn't create video encoding context\n");
		return false;
	}
	ctx->av_packet = av_packet_alloc();
	if (!ctx->av_packet) {
		fprintf(stderr, "couldn't allocate video packet\n");
		return false;
	}
	ctx->video_encoder->codec_id = fmt->video_codec;
	ctx->video_encoder->bit_rate = (int64_t)quality * width * height;
	ctx->video_encoder->width = width;
	ctx->video_encoder->height = height;
	ctx->video_encoder->time_base = ctx->video_stream->time_base = (AVRational){1, fps};
	ctx->video_encoder->gop_size = 12;
	ctx->video_encoder->pix_fmt = AV_PIX_FMT_YUV420P;
	if (ctx->avf_context->oformat->flags & AVFMT_GLOBALHEADER)
		ctx->video_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	err = avcodec_open2(ctx->video_encoder, video_codec, NULL);
	if (err < 0) {
		fprintf(stderr, "error: avcodec_open2: %s\n", av_err2str(err));
		return false;
	}
	err = avcodec_parameters_from_context(ctx->video_stream->codecpar, ctx->video_encoder);
	if (err < 0) {
		fprintf(stderr, "error: avcodec_parameters_from_context: %s\n", av_err2str(err));
		return false;
	}
	ctx->video_frame = av_frame_alloc();
	if (!ctx->video_frame) {
		fprintf(stderr, "couldn't allocate video frame\n");
		return false;
	}
	ctx->video_frame->format = AV_PIX_FMT_YUV420P;
	ctx->video_frame->width = ctx->video_encoder->width;
	ctx->video_frame->height = ctx->video_encoder->height;
	err = av_frame_get_buffer(ctx->video_frame, 0);
	if (err < 0) {
		fprintf(stderr, "error: av_frame_get_buffer: %s\n", av_err2str(err));
		return false;
	}
	// av_dump_format(state->avf_context, 0, filename, 1);
	err = avio_open(&ctx->avf_context->pb, filename, AVIO_FLAG_WRITE);
	if (err < 0) {
		fprintf(stderr, "error: avio_open: %s\n", av_err2str(err));
		return false;
	}
	err = avformat_write_header(ctx->avf_context, NULL);
	if (err < 0) {
		fprintf(stderr, "error: avformat_write_header: %s\n", av_err2str(err));
		return false;
	}
	if (ctx->audio_mutex) {
		SDL_LockMutex(ctx->audio_mutex);
		// we're just using this audio stream as a queue really.
		ctx->audio_stream = SDL_NewAudioStream(AUDIO_S16, 2, 44100, AUDIO_S16, 2, 44100);
		SDL_UnlockMutex(ctx->audio_mutex);
	}
	ctx->recording = true;
	ctx->next_video_pts = 0;
	ctx->start_time = get_time_double();
	return true;
}


static bool write_frame(VideoContext *ctx, AVCodecContext *encoder, AVStream *stream, AVFrame *frame) {
	int err = avcodec_send_frame(encoder, frame);
	if (err < 0) {
		fprintf(stderr, "error: avcodec_send_frame: %s\n", av_err2str(err));
		return false;
	}
	while (true) {
		err = avcodec_receive_packet(encoder, ctx->av_packet);
		if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
			break;
		}
		if (err < 0) {
			fprintf(stderr, "error: avcodec_receive_packet: %s\n", av_err2str(err));
			return false;
		}
		ctx->av_packet->stream_index = stream->index;
		av_packet_rescale_ts(ctx->av_packet, encoder->time_base, stream->time_base);
		err = av_interleaved_write_frame(ctx->avf_context, ctx->av_packet);
		if (err < 0) {
			fprintf(stderr, "error: av_interleaved_write_frame: %s\n", av_err2str(err));
			return false;
		}
	}
	return true;
}

bool video_submit_frame(VideoContext *ctx, Camera *camera) {
	if (!ctx || !camera || !ctx->recording) return false;
	double curr_time = get_time_double();
	double time_since_start = curr_time - ctx->start_time;
	int64_t video_pts = (int64_t)(time_since_start
		* ctx->video_encoder->time_base.den
		/ ctx->video_encoder->time_base.num);
	if (ctx->audio_mutex) {
		SDL_LockMutex(ctx->audio_mutex);
		char buf[4096];
		int n;
		while ((n = SDL_AudioStreamGet(ctx->audio_stream, buf, sizeof buf))) {
			printf("%d\n",n);
		}
		SDL_UnlockMutex(ctx->audio_mutex);
	}
	if (video_pts >= ctx->next_video_pts) {
		int err = av_frame_make_writable(ctx->video_frame);
		if (err < 0) {
			fprintf(stderr, "error: av_frame_make_writable: %s\n", av_err2str(err));
			return false;
		}
		ctx->video_frame->pts = video_pts;
		camera_copy_to_av_frame(camera, ctx->video_frame);
		write_frame(ctx, ctx->video_encoder, ctx->video_stream, ctx->video_frame);
		ctx->next_video_pts = video_pts + 1;
	}
	return true;
}

bool video_is_recording(VideoContext *ctx) {
	if (!ctx) return false;
	return ctx->recording;
}

void video_stop(VideoContext *ctx) {
	if (!ctx) return;
	if (ctx->recording) {
		if (ctx->audio_mutex) {
			SDL_LockMutex(ctx->audio_mutex);
			SDL_FreeAudioStream(ctx->audio_stream);
			ctx->audio_stream = NULL;
			SDL_UnlockMutex(ctx->audio_mutex);
		}
		ctx->recording = false;
		// flush video encoder
		write_frame(ctx, ctx->video_encoder, ctx->video_stream, NULL);
		int err = av_write_trailer(ctx->avf_context);
		if (err < 0) {
			fprintf(stderr, "error: av_write_trailer: %s\n", av_err2str(err));
		}
		avio_closep(&ctx->avf_context->pb);
	}
	if (ctx->video_encoder) {
		avcodec_free_context(&ctx->video_encoder);
	}
	if (ctx->video_frame) {
		av_frame_free(&ctx->video_frame);
	}
	if (ctx->avf_context) {
		if (ctx->avf_context->pb) {
			avio_closep(&ctx->avf_context->pb);
		}
		avformat_free_context(ctx->avf_context);
		ctx->avf_context = NULL;
	}
	if (ctx->av_packet) {
		av_packet_free(&ctx->av_packet);
	}
}

void video_quit(VideoContext *ctx) {
	if (!ctx) return;
	video_stop(ctx);
	if (ctx->audio_thread) {
		SDL_LockMutex(ctx->audio_mutex);
		ctx->audio_quit = true;
		SDL_UnlockMutex(ctx->audio_mutex);
		SDL_WaitThread(ctx->audio_thread, NULL);
	}
	if (ctx->audio_stream)
		SDL_FreeAudioStream(ctx->audio_stream);
	if (ctx->audio_mutex)
		SDL_DestroyMutex(ctx->audio_mutex);
	free(ctx);
}

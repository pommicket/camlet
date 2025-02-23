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
	bool recording;
	double start_time;
	double last_frame_time;
	AVFormatContext *avf_context;
	AVCodecContext *video_encoder;
	AVFrame *video_frame;
	AVPacket *av_packet;
	AVStream *video_stream;
	int64_t next_video_pts;
	pa_simple *pulseaudio;
};

VideoContext *video_init(void) {
	VideoContext *ctx = calloc(1, sizeof(VideoContext));
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
	pa_sample_spec audio_format = {
		.format = PA_SAMPLE_S16NE,
		.channels = 2,
		.rate = 44100
	};
	// NOTE: SDL2 audio capture is currently broken
	//  https://github.com/libsdl-org/SDL/issues/9706
	// once SDL3 becomes commonplace enough, we can switch to that for capturing audio.
	ctx->pulseaudio = pa_simple_new(NULL, "camlet", PA_STREAM_RECORD, NULL,
		"microphone", &audio_format, NULL, NULL, &err);
	if (!ctx->pulseaudio) {
		fprintf(stderr, "%s", pa_strerror(err));
	}
	pa_simple_read(ctx->pulseaudio, (char[1]){0}, 1, &err);
	ctx->recording = true;
	ctx->next_video_pts = 0;
	ctx->start_time = ctx->last_frame_time = get_time_double();
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
	double time_since_last_frame = curr_time - ctx->last_frame_time;
	ctx->last_frame_time = curr_time;
	int64_t video_pts = (int64_t)(time_since_start
		* ctx->video_encoder->time_base.den
		/ ctx->video_encoder->time_base.num);
	size_t audio_size = ctx->pulseaudio
		? (size_t)(time_since_last_frame * 44100 * 2 * sizeof(int16_t))
		: 0;
	int err;
	while (audio_size > 0 && audio_size < SIZE_MAX / 2 /* just in case time goes backwards somehow */) {
		uint8_t buf[4096];
		size_t n = audio_size > sizeof buf ? sizeof buf : audio_size;
		if (pa_simple_read(ctx->pulseaudio, buf, n, &err) < 0) {
			static atomic_flag warned = ATOMIC_FLAG_INIT;
			if (!atomic_flag_test_and_set_explicit(&warned, memory_order_relaxed)) {
				fprintf(stderr, "error: pa_simple_read: %s\n", pa_strerror(err));
			}
			break;
		}
		audio_size -= n;
	}
	if (video_pts >= ctx->next_video_pts) {
		err = av_frame_make_writable(ctx->video_frame);
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
		if (ctx->pulseaudio) {
			pa_simple_free(ctx->pulseaudio);
			ctx->pulseaudio = NULL;
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
	free(ctx);
}

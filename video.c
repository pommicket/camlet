#include "video.h"

#include <stdatomic.h>
#include <threads.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <SDL.h>
#include <unistd.h>

#include "log.h"
#include "camera.h"

// no real harm in making this bigger, other than increased memory usage.
#define AUDIO_QUEUE_SIZE ((size_t)128 << 10)

struct VideoContext {
	double start_time;
	AVFormatContext *avf_context;
	AVCodecContext *video_encoder;
	AVCodecContext *audio_encoder;
	AVFrame *video_frame;
	AVFrame *audio_frame;
	int audio_frame_samples;
	AVPacket *av_packet;
	AVStream *video_stream;
	AVStream *audio_stream;
	int64_t next_video_pts;
	int64_t next_audio_pts;
	SDL_AudioDeviceID audio_device;
	bool recording;
	// ring buffer of audio data.
	float audio_queue[AUDIO_QUEUE_SIZE];
	atomic_uint_fast32_t audio_head;
	atomic_uint_fast32_t audio_tail;
	char _unused1[128]; // reduce false sharing
};

// NOTE: SDL2 pulseaudio capture is broken on some versions of SDL 2.30: https://github.com/libsdl-org/SDL/issues/9706
static void audio_callback(void *data, Uint8 *stream_u8, int len) {
	VideoContext *ctx = data;
	const float *stream = (const float *)stream_u8;
	// this call already happens-after any earlier writes to audio_tail, so relaxed is fine.
	uint32_t tail = atomic_load_explicit(&ctx->audio_tail, memory_order_relaxed);
	uint32_t head = atomic_load(&ctx->audio_head);
	if ((tail - head + AUDIO_QUEUE_SIZE) % AUDIO_QUEUE_SIZE > AUDIO_QUEUE_SIZE * 3 / 4) {
		static int warned;
		if (warned < 10) {
			log_warning("audio overrun");
			warned++;
		}
	} else {
		const uint32_t nfloats = (uint32_t)len / sizeof(float);
		if (tail + nfloats <= AUDIO_QUEUE_SIZE) {
			// easy case
			memcpy(&ctx->audio_queue[tail], stream, len);
			tail += nfloats;
		} else {
			// "wrap around" case
			memcpy(&ctx->audio_queue[tail], stream, (AUDIO_QUEUE_SIZE - tail) * sizeof(float));
			memcpy(&ctx->audio_queue[0], &stream[AUDIO_QUEUE_SIZE - tail], (tail + nfloats - AUDIO_QUEUE_SIZE) * sizeof(float));
			tail = tail + nfloats - AUDIO_QUEUE_SIZE;
		}
	}
	atomic_store(&ctx->audio_tail, tail);
}


VideoContext *video_init(void) {
	VideoContext *ctx = calloc(1, sizeof(VideoContext));
	if (!ctx) return NULL;
	atomic_init(&ctx->audio_head, 0);
	atomic_init(&ctx->audio_tail, 0);
	
	SDL_AudioSpec desired = {
		.channels = 2,
		.freq = 44100,
		.format = AUDIO_F32,
		.samples = 2048,
		.callback = audio_callback,
		.userdata = ctx,
	}, obtained = {0};
	ctx->audio_device = SDL_OpenAudioDevice(NULL, 1, &desired, &obtained, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
	if (!ctx->audio_device) {
		log_error("couldn't create audio device: %s", SDL_GetError());
	}
	return ctx;
}

bool video_start(VideoContext *ctx, const char *filename, int32_t width, int32_t height, int fps, int quality) {
	if (!ctx) return false;
	if (ctx->recording) {
		return true;
	}
	video_stop(ctx);
	bool have_audio = false;
	int err = avformat_alloc_output_context2(&ctx->avf_context, NULL, NULL, filename);
	if (!ctx->avf_context) {
		log_error("avformat_alloc_output_context2 \"%s\": %s", filename, av_err2str(err));
		return false;
	}
	const AVOutputFormat *fmt = ctx->avf_context->oformat;
	const AVCodec *video_codec = avcodec_find_encoder(fmt->video_codec);
	if (!video_codec) {
		log_error("couldn't find encoder for video codec %s", avcodec_get_name(fmt->video_codec));
		return false;
	}
	ctx->video_stream = avformat_new_stream(ctx->avf_context, NULL);
	if (!ctx->video_stream) {
		log_error("avformat_new_stream (audio): %s", av_err2str(err));
		return false;
	}
	ctx->video_stream->id = 0;
	ctx->video_encoder = avcodec_alloc_context3(video_codec);
	if (!ctx->video_encoder) {
		log_error("couldn't create video encoding context for codec %s", avcodec_get_name(fmt->video_codec));
		return false;
	}
	ctx->av_packet = av_packet_alloc();
	if (!ctx->av_packet) {
		log_error("couldn't allocate video packet");
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
		log_error("avcodec_open2 for video codec %s: %s", avcodec_get_name(fmt->video_codec), av_err2str(err));
		return false;
	}
	err = avcodec_parameters_from_context(ctx->video_stream->codecpar, ctx->video_encoder);
	if (err < 0) {
		log_error("avcodec_parameters_from_context for video codec %s: %s", avcodec_get_name(fmt->video_codec), av_err2str(err));
		return false;
	}
	ctx->video_frame = av_frame_alloc();
	if (!ctx->video_frame) {
		log_error("couldn't allocate video frame");
		return false;
	}
	ctx->video_frame->format = AV_PIX_FMT_YUV420P;
	ctx->video_frame->width = ctx->video_encoder->width;
	ctx->video_frame->height = ctx->video_encoder->height;
	err = av_frame_get_buffer(ctx->video_frame, 0);
	if (err < 0) {
		log_error("av_frame_get_buffer for video: %s", av_err2str(err));
		return false;
	}
	err = avio_open(&ctx->avf_context->pb, filename, AVIO_FLAG_WRITE);
	if (err < 0) {
		log_error("avio_open \"%s\": %s", filename, av_err2str(err));
		return false;
	}
	const AVCodec *audio_codec = avcodec_find_encoder(fmt->audio_codec);
	if (!audio_codec) {
		log_error("avcodec_find_encoder for audio codec %s: %s", avcodec_get_name(fmt->audio_codec), av_err2str(err));
		goto no_audio;
	}
	ctx->audio_encoder = avcodec_alloc_context3(audio_codec);
	if (!ctx->audio_encoder) {
		log_error("avcodec_alloc_context3 for audio codec %s: %s", avcodec_get_name(fmt->audio_codec), av_err2str(err));
		goto no_audio;
	}
	// only FLTP is supported by AAC encoder
	ctx->audio_encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
	ctx->audio_encoder->bit_rate = (int64_t)192 * 1024;
	ctx->audio_encoder->sample_rate = 44100;
	static const AVChannelLayout channel_layout = AV_CHANNEL_LAYOUT_STEREO;
	av_channel_layout_copy(&ctx->audio_encoder->ch_layout, &channel_layout);
	if (ctx->avf_context->oformat->flags & AVFMT_GLOBALHEADER)
		ctx->audio_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	err = avcodec_open2(ctx->audio_encoder, audio_codec, NULL);
	if (err < 0) {
		log_error("avcodec_open2 for audio codec %s: %s", avcodec_get_name(fmt->audio_codec), av_err2str(err));
		goto no_audio;
	}
	ctx->audio_frame_samples = audio_codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE
		? 4096
		: ctx->audio_encoder->frame_size;
	ctx->audio_frame = av_frame_alloc();
	if (!ctx->audio_frame) {
		log_error("couldn't allocate audio frame");
		goto no_audio;
	}
	ctx->audio_frame->format = AV_SAMPLE_FMT_FLTP;
	av_channel_layout_copy(&ctx->audio_frame->ch_layout, &channel_layout);
	ctx->audio_frame->sample_rate = 44100;
	ctx->audio_frame->nb_samples = ctx->audio_frame_samples;
	err = av_frame_get_buffer(ctx->audio_frame, 0);
	if (err < 0) {
		log_error("av_frame_get_buffer (audio): %s", av_err2str(err));
		goto no_audio;
	}

	// create stream last so that if stuff above fails we don't have a broken stream in the avformat context
	ctx->audio_stream = avformat_new_stream(ctx->avf_context, audio_codec);
	if (!ctx->audio_stream) {
		log_error("avformat_new_stream (audio): %s", av_err2str(err));
		goto no_audio;
	}
	ctx->audio_stream->id = 1;
	ctx->audio_stream->time_base = (AVRational){1, 44100};
	err = avcodec_parameters_from_context(ctx->audio_stream->codecpar, ctx->audio_encoder);
	if (err < 0) {
		log_error("avcodec_parameters_from_context (audio): %s", av_err2str(err));
		goto no_audio;
	}
	have_audio = true;
no_audio:
	err = avformat_write_header(ctx->avf_context, NULL);
	if (err < 0) {
		log_error("avformat_write_header: %s", av_err2str(err));
		return false;
	}
	atomic_store(&ctx->audio_head, 0);
	ctx->next_video_pts = 0;
	ctx->next_audio_pts = 0;
	ctx->recording = true;
	ctx->start_time = get_time_double();
	if (have_audio) {
		// start recording audio
		SDL_PauseAudioDevice(ctx->audio_device, 0);
	}
	return true;
}


static bool write_frame(VideoContext *ctx, AVCodecContext *encoder, AVStream *stream, AVFrame *frame) {
	int err = avcodec_send_frame(encoder, frame);
	if (err < 0) {
		log_error("avcodec_send_frame (stream %d): %s", stream->index, av_err2str(err));
		return false;
	}
	while (true) {
		err = avcodec_receive_packet(encoder, ctx->av_packet);
		if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
			break;
		}
		if (err < 0) {
			log_error("avcodec_receive_packet (stream %d): %s", stream->index, av_err2str(err));
			return false;
		}
		ctx->av_packet->stream_index = stream->index;
		av_packet_rescale_ts(ctx->av_packet, encoder->time_base, stream->time_base);
		err = av_interleaved_write_frame(ctx->avf_context, ctx->av_packet);
		if (err < 0) {
			log_error("av_interleaved_write_frame (stream %d): %s", stream->index, av_err2str(err));
			return false;
		}
	}
	return true;
}

bool video_submit_frame(VideoContext *ctx, Camera *camera) {
	if (!ctx || !camera || !ctx->recording) return false;
	double curr_time = get_time_double();
	double time_since_start = curr_time - ctx->start_time;
	if (ctx->audio_device) {
		// process audio
		// only this thread writes to head, so relaxed is fine.
		uint32_t head = atomic_load_explicit(&ctx->audio_head, memory_order_relaxed);
		uint32_t tail = atomic_load(&ctx->audio_tail);
		while (true) {
			int err = av_frame_make_writable(ctx->audio_frame);
			if (err < 0) {
				log_error("av_frame_make_writable (video): %s", av_err2str(err));
				break;
			}
			ctx->audio_frame->pts = ctx->next_audio_pts;
			uint32_t nfloats = (uint32_t)ctx->audio_frame_samples * 2;
			bool frame_ready = false;
			if (head + nfloats < AUDIO_QUEUE_SIZE) {
				// easy case
				frame_ready = head + nfloats <= tail || head > tail /* tail wrapped around */;
				if (frame_ready) {
					for (uint32_t s = 0; s < nfloats; s++) {
						((float *)ctx->audio_frame->data[s % 2])[s / 2] = ctx->audio_queue[head + s];
					}
					head += nfloats;
				}
			} else {
				// "wrap around" case
				frame_ready = head + nfloats - AUDIO_QUEUE_SIZE <= tail && tail < head;
				if (frame_ready) {
					for (uint32_t s = 0; s < AUDIO_QUEUE_SIZE - head; s++) {
						((float *)ctx->audio_frame->data[s % 2])[s / 2] = ctx->audio_queue[head + s];
					}
					for (uint32_t s = 0; s < head + nfloats - AUDIO_QUEUE_SIZE; s++) {
						uint32_t i = AUDIO_QUEUE_SIZE - head + s;
						((float *)ctx->audio_frame->data[i % 2])[i / 2] = ctx->audio_queue[s];
					}
					head = head + nfloats - AUDIO_QUEUE_SIZE;
				}
			}
			if (frame_ready) {
				ctx->next_audio_pts += ctx->audio_frame_samples;
				write_frame(ctx, ctx->audio_encoder, ctx->audio_stream, ctx->audio_frame);
			} else {
				break;
			}
		}
		atomic_store(&ctx->audio_head, head);
	}
	// process video
	int64_t video_pts = (int64_t)(time_since_start
		* ctx->video_encoder->time_base.den
		/ ctx->video_encoder->time_base.num);
	if (video_pts >= ctx->next_video_pts) {
		int err = av_frame_make_writable(ctx->video_frame);
		if (err < 0) {
			log_error("av_frame_make_writable (audio): %s", av_err2str(err));
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
		SDL_PauseAudioDevice(ctx->audio_device, 1);
		// block until callback finishes.
		SDL_LockAudioDevice(ctx->audio_device);
		SDL_UnlockAudioDevice(ctx->audio_device);
		atomic_store(&ctx->audio_head, 0);
		atomic_store(&ctx->audio_tail, 0);
		ctx->recording = false;
		// flush video encoder
		write_frame(ctx, ctx->video_encoder, ctx->video_stream, NULL);
		// flush audio encoder
		write_frame(ctx, ctx->audio_encoder, ctx->audio_stream, NULL);
		int err = av_write_trailer(ctx->avf_context);
		if (err < 0) {
			log_error("av_write_trailer: %s", av_err2str(err));
		}
		avio_closep(&ctx->avf_context->pb);
	}
	if (ctx->video_encoder)
		avcodec_free_context(&ctx->video_encoder);
	if (ctx->audio_encoder)
		avcodec_free_context(&ctx->audio_encoder);
	if (ctx->video_frame)
		av_frame_free(&ctx->video_frame);
	if (ctx->audio_frame)
		av_frame_free(&ctx->audio_frame);
	if (ctx->avf_context) {
		if (ctx->avf_context->pb) {
			avio_closep(&ctx->avf_context->pb);
		}
		avformat_free_context(ctx->avf_context);
		ctx->avf_context = NULL;
	}
	if (ctx->av_packet)
		av_packet_free(&ctx->av_packet);
}

void video_quit(VideoContext *ctx) {
	if (!ctx) return;
	video_stop(ctx);
	if (ctx->audio_device) {
		SDL_CloseAudioDevice(ctx->audio_device);
	}
	free(ctx);
}

#include "video.h"

#include <stdatomic.h>
#include <threads.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <unistd.h>

#include "log.h"
#include "camera.h"

// no real harm in making this bigger, other than increased memory usage.
#define AUDIO_QUEUE_SIZE ((size_t)128 << 10)
#define AUDIO_NOT_RECORDING (UINT32_MAX)
#define AUDIO_QUIT (UINT32_MAX - 1)

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
	thrd_t audio_thread;
	bool audio_thread_created;
	bool recording;
	// ring buffer of audio data.
	float audio_queue[AUDIO_QUEUE_SIZE];
	atomic_uint_fast32_t audio_head;
	atomic_uint_fast32_t audio_tail;
	char _unused[128]; // reduce false sharing
};

// NOTE: SDL2 pulseaudio capture is broken on some versions: https://github.com/libsdl-org/SDL/issues/9706
//      When SDL3 is widespread enough (e.g. available on Debian stable), we can switch to that for capturing.
static int audio_thread(void *data) {
	VideoContext *ctx = data;
	pa_sample_spec audio_format = {
		.format = PA_SAMPLE_FLOAT32NE,
		.rate = 44100,
		.channels = 2,
	};
	// by default pulseaudio uses a crazy large buffer, like 500KB,
	// and doesn't send any audio until it's full.
	const pa_buffer_attr buffer_attr = {
		.maxlength = 8192,
		.fragsize = 4096,
	};
	int err = 0;
	pa_simple *pulseaudio = pa_simple_new(NULL, "camlet", PA_STREAM_RECORD, NULL,
		"microphone", &audio_format, NULL, &buffer_attr, &err);
	if (!pulseaudio) {
		log_error("couldn't connect to pulseaudio: %s", pa_strerror(err));
		return -1;
	}
	uint32_t warned[2] = {0};
	bool quit = false;
	// only this thread writes to ctx->tail, so we can have a local variable which mirrors it.
	uint32_t tail = AUDIO_NOT_RECORDING;
	while (!quit) {
		float buf[1024];
		int result = pa_simple_read(pulseaudio, buf, sizeof buf, &err);
		uint32_t head = atomic_load(&ctx->audio_head);
		if (head == AUDIO_NOT_RECORDING) {
			// not recording
			if (tail != AUDIO_NOT_RECORDING) {
				tail = AUDIO_NOT_RECORDING;
				atomic_store(&ctx->audio_tail, tail);
			}
			continue;
		} else if (head == AUDIO_QUIT) {
			break;
		} else if (tail == AUDIO_NOT_RECORDING) {
			// reset tail now that we are recording
			tail = 0;
		}
		if ((tail - head + AUDIO_QUEUE_SIZE) % AUDIO_QUEUE_SIZE > AUDIO_QUEUE_SIZE * 3 / 4) {
			if (warned[0] < 10) {
				log_warning("audio overrun");
				warned[0]++;
			}
		} else if (result >= 0) {
			const uint32_t nfloats = sizeof buf / sizeof(float);
			if (tail + nfloats <= AUDIO_QUEUE_SIZE) {
				// easy case
				memcpy(&ctx->audio_queue[tail], buf, sizeof buf);
				tail += nfloats;
			} else {
				// "wrap around" case
				memcpy(&ctx->audio_queue[tail], buf, (AUDIO_QUEUE_SIZE - tail) * sizeof(float));
				memcpy(&ctx->audio_queue[0], &buf[AUDIO_QUEUE_SIZE - tail], (tail + nfloats - AUDIO_QUEUE_SIZE) * sizeof(float));
				tail = tail + nfloats - AUDIO_QUEUE_SIZE;
			}
		} else {
			if (!warned[1]) {
				log_error("pa_simple_read: %s", pa_strerror(err));
				warned[1]++;
			}
		}
		atomic_store(&ctx->audio_tail, tail);
	}
	pa_simple_free(pulseaudio);
	return 0;
}


VideoContext *video_init(void) {
	VideoContext *ctx = calloc(1, sizeof(VideoContext));
	if (!ctx) return NULL;
	atomic_init(&ctx->audio_head, AUDIO_NOT_RECORDING);
	atomic_init(&ctx->audio_tail, AUDIO_NOT_RECORDING);
	if (thrd_create(&ctx->audio_thread, audio_thread, ctx) == thrd_success) {
		ctx->audio_thread_created = true;
	} else {
		log_perror("couldn't create audio thread");
	}
	return ctx;
}

bool video_start(VideoContext *ctx, const char *filename, int32_t width, int32_t height, int fps, int quality) {
	if (!ctx) return false;
	if (ctx->recording) {
		return true;
	}
	video_stop(ctx);
	// wait for capture thread to acknowledge that we have stopped recording
	if (ctx->audio_thread_created) while (true) {
		if (atomic_load(&ctx->audio_tail) == AUDIO_NOT_RECORDING) break;
		usleep(1000);
	}
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
	if (ctx->audio_thread_created) {
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
			if (tail == AUDIO_NOT_RECORDING) {
				// capture thread doesn't even know we're recording yet
			} else if (head + nfloats < AUDIO_QUEUE_SIZE) {
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
		atomic_store(&ctx->audio_head, AUDIO_NOT_RECORDING);
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
	if (ctx->audio_thread_created) {
		atomic_store(&ctx->audio_head, AUDIO_QUIT);
		if (thrd_join(ctx->audio_thread, NULL) != thrd_success) {
			log_perror("thrd_join");
		}
	}
	free(ctx);
}

#include "video.h"

#include <stdatomic.h>
#include <threads.h>
#include <SDL.h>
#include <unistd.h>
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <vpx/vp8cx.h>

#include "log.h"
#include "camera.h"

// no real harm in making this bigger, other than increased memory usage.
#define AUDIO_QUEUE_SIZE ((size_t)128 << 10)

struct VideoContext {
	double start_time;
	ogg_stream_state video_stream, audio_stream;
	vorbis_dsp_state vorbis;
	vorbis_info vorbis_info;
	vorbis_block vorbis_block;
	vpx_codec_ctx_t vpx;
	vpx_image_t vpx_image;
	int64_t next_video_pts;
	int64_t video_packetno;
	int framerate;
	SDL_AudioDeviceID audio_device;
	FILE *outfile;
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

static bool write_packet_to_stream(VideoContext *ctx, ogg_stream_state *stream, ogg_packet *packet) {
	if (ogg_stream_packetin(stream, packet) != 0) {
		log_error("ogg_stream_packetin failed");
		return false;
	}
	ogg_page page;
	while (ogg_stream_pageout(stream, &page) != 0) {
		fwrite(page.header, 1, page.header_len, ctx->outfile);
		fwrite(page.body, 1, page.body_len, ctx->outfile);
	}
	if (ferror(ctx->outfile)) {
		log_error("error writing video output");
		return false;
	}
	return true;
}

// inverse of vp8_gptopts in  https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/oggparsevp8.c
// see also: https://github.com/FFmpeg/FFmpeg/blob/99e2af4e7837ca09b97d93a562dc12947179fc48/libavformat/oggenc.c#L671
static uint64_t vp8_pts_to_gp(int64_t pts, bool is_key_frame) {
	return (uint64_t)pts << 32 | (uint64_t)!is_key_frame << 3;
}

bool video_start(VideoContext *ctx, const char *filename, int32_t width, int32_t height, int fps, int quality) {
	if (!ctx) return false;
	if (ctx->recording) {
		return true;
	}
	video_stop(ctx);
	ctx->framerate = fps;
	ctx->outfile = fopen(filename, "wb");
	if (!ctx->outfile) {
		log_perror("couldn't create %s", filename);
	}
	struct timespec ts = {1, 1};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int serial_number = (int)((int32_t)ts.tv_nsec + 1000000000 * ((int32_t)ts.tv_sec % 2));
	if (ogg_stream_init(&ctx->video_stream, serial_number) < 0) {
		log_error("ogg_stream_init(video_stream) failed");
		return false;
	}
	if (ogg_stream_init(&ctx->audio_stream, serial_number + 1) < 0) {
		log_error("ogg_stream_init(audio_stream) failed");
		return false;
	}
	vpx_codec_enc_cfg_t cfg = {0};
	// NOTE: vp9 encoder seems to be much slower and OggVP9 isn't a thing (yet)
	vpx_codec_iface_t *vp8 = vpx_codec_vp8_cx();
	int err = vpx_codec_enc_config_default(vp8, &cfg, 0);
	if (err != 0) {
		log_error("vpx_codec_enc_config_default: %s", vpx_codec_err_to_string(err));
		return false;
	}
	cfg.g_w = width;
	cfg.g_h = height;
	cfg.g_timebase.num = 1;
	cfg.g_timebase.den = fps;
	cfg.rc_target_bitrate = (unsigned)quality * (unsigned)width * (unsigned)height;
	err = vpx_codec_enc_init(&ctx->vpx, vp8, &cfg, 0);
	if (err != 0) {
		log_error("vpx_codec_enc_init: %s", vpx_codec_err_to_string(err));
		return false;
	}
	if (!vpx_img_alloc(&ctx->vpx_image, VPX_IMG_FMT_I420, width, height, 1)) {
		log_error("couldn't allocate VPX image");
		return false;
	}
	// I can't find any documentation of OggVP8
	//  This was pieced together from ogg_build_vp8_headers in
	//   https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/oggenc.c
	typedef struct {
		char magic[5];
		uint8_t stream_type;
		uint8_t version[2];
		// doesn't seem very forwards-thinking to have these be 16-bit. oh well.
		uint16_t width;
		uint16_t height;
		uint8_t sample_aspect_ratio_num[3];
		uint8_t sample_aspect_ratio_den[3];
		// not aligned to 4 bytes ):
		uint16_t framerate_num_hi;
		uint16_t framerate_num_lo;
		uint16_t framerate_den_hi;
		uint16_t framerate_den_lo;
	} OggVP8Header;
	if (width > UINT16_MAX || height > UINT16_MAX) {
		log_error("video resolution too high");
		return false;
	}
	OggVP8Header header = {
		.magic = "OVP80",
		.stream_type = 1,
		.version = {1, 0},
		// big-endian for some reason....
		.width = SDL_SwapBE16((uint16_t)width),
		.height = SDL_SwapBE16((uint16_t)height),
		.sample_aspect_ratio_num = {0, 0, 1},
		.sample_aspect_ratio_den = {0, 0, 1},
		.framerate_num_lo = SDL_SwapBE16((uint16_t)fps),
		.framerate_den_lo = SDL_SwapBE16(1),
	};
	ogg_packet packet = {
		.packet = (uint8_t *)&header,
		.bytes = sizeof header,
		.granulepos = vp8_pts_to_gp(0, false),
		.b_o_s = true,
		.e_o_s = false,
	};
	write_packet_to_stream(ctx, &ctx->video_stream, &packet);
	bool have_audio = false;
	vorbis_info_init(&ctx->vorbis_info);
	if ((err = vorbis_encode_init_vbr(&ctx->vorbis_info, 2, 44100, 0.9f)) != 0) {
		log_error("vorbis_encode_init_vbr failed (error %d)", err);
		goto no_audio;
	}
	if ((err = vorbis_encode_setup_init(&ctx->vorbis_info)) != 0) {
		log_error("vorbis_encode_setup_init failed (error %d)", err);
		goto no_audio;
	}
	if (vorbis_analysis_init(&ctx->vorbis, &ctx->vorbis_info) != 0) {
		log_error("vorbis_analysis_init failed");
		goto no_audio;
	}
	if (vorbis_block_init(&ctx->vorbis, &ctx->vorbis_block) != 0) {
		log_error("vorbis_block_init failed");
		goto no_audio;
	}
	vorbis_comment comments = {0};
	vorbis_comment_init(&comments);
	ogg_packet header_packets[3] = {0};
	if (vorbis_analysis_headerout(&ctx->vorbis, &comments,
		&header_packets[0], &header_packets[1], &header_packets[2]) != 0) {
		log_error("vorbis_analysis_headerout failed");
		goto no_audio;
	}
	vorbis_comment_clear(&comments);
	for (int i = 0; i < 3; i++) {
		if (!write_packet_to_stream(ctx, &ctx->audio_stream, &header_packets[i])) {
			goto no_audio;
		}
	}
	have_audio = true;
no_audio:
	atomic_store(&ctx->audio_head, 0);
	ctx->recording = true;
	ctx->start_time = get_time_double();
	ctx->next_video_pts = 0;
	ctx->video_packetno = 1;
	if (have_audio) {
		// start recording audio
		SDL_PauseAudioDevice(ctx->audio_device, 0);
	}
	return true;
}


static void write_video_frame(VideoContext *ctx, vpx_image_t *image, int64_t pts) {
	int err = vpx_codec_encode(&ctx->vpx, image, pts, 1, 0, 1000000 / (ctx->framerate * 2));
	if (err != 0) {
		log_error("vpx_codec_encode: %s", vpx_codec_err_to_string(err));
	}
	const vpx_codec_cx_pkt_t *pkt = NULL;
	vpx_codec_iter_t iter = NULL;
	while ((pkt = vpx_codec_get_cx_data(&ctx->vpx, &iter))) {
		if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
		ogg_packet oggp = {
			.packet = pkt->data.frame.buf,
			.bytes = pkt->data.frame.sz,
			.granulepos = vp8_pts_to_gp(pkt->data.frame.pts, pkt->data.frame.flags & VPX_FRAME_IS_KEY),
			.b_o_s = false,
			.packetno = ctx->video_packetno++,
			.e_o_s = false,
		};
		write_packet_to_stream(ctx, &ctx->video_stream, &oggp);
	}
}


static void write_audio_frame(VideoContext *ctx, int nsamples) {
	int err = vorbis_analysis_wrote(&ctx->vorbis, nsamples);
	if (err != 0) {
		log_error("vorbis_analysis_wrote failed (error %d)", err);
	}
	while ((err = vorbis_analysis_blockout(&ctx->vorbis, &ctx->vorbis_block)) > 0) {
		if ((err = vorbis_analysis(&ctx->vorbis_block, NULL)) != 0) {
			log_error("vorbis_analysis failed (error %d)", err);
		}
		if ((err = vorbis_bitrate_addblock(&ctx->vorbis_block)) != 0) {
			log_error("vorbis_bitrate_addblock failed (error %d)", err);
		}
		ogg_packet oggp;
		while ((err = vorbis_bitrate_flushpacket(&ctx->vorbis, &oggp)) > 0) {
			write_packet_to_stream(ctx, &ctx->audio_stream, &oggp);
		}
		if (err < 0) {
			log_error("vorbis_bitrate_flushpacket failed (error %d)", err);
		}
	}
	if (err < 0) {
		log_error("vorbis_analysis_blockout failed (error %d)", err);
	}
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
			uint32_t audio_frame_samples = 1024; // value recommended by vorbis
			uint32_t nfloats = (uint32_t)audio_frame_samples * 2;
			bool frame_ready = false;
			if (head + nfloats < AUDIO_QUEUE_SIZE) {
				// easy case
				frame_ready = head + nfloats <= tail || head > tail /* tail wrapped around */;
				if (frame_ready) {
					float **buffer = vorbis_analysis_buffer(&ctx->vorbis, audio_frame_samples);
					for (uint32_t s = 0; s < nfloats; s++) {
						buffer[s % 2][s / 2] = ctx->audio_queue[head + s];
					}
					head += nfloats;
				}
			} else {
				// "wrap around" case
				frame_ready = head + nfloats - AUDIO_QUEUE_SIZE <= tail && tail < head;
				if (frame_ready) {
					float **buffer = vorbis_analysis_buffer(&ctx->vorbis, audio_frame_samples);
					for (uint32_t s = 0; s < AUDIO_QUEUE_SIZE - head; s++) {
						buffer[s % 2][s / 2] = ctx->audio_queue[head + s];
					}
					for (uint32_t s = 0; s < head + nfloats - AUDIO_QUEUE_SIZE; s++) {
						uint32_t i = AUDIO_QUEUE_SIZE - head + s;
						buffer[i % 2][i / 2] = ctx->audio_queue[s];
					}
					head = head + nfloats - AUDIO_QUEUE_SIZE;
				}
			}
			if (frame_ready) {
				write_audio_frame(ctx, audio_frame_samples);
			} else {
				break;
			}
		}
		atomic_store(&ctx->audio_head, head);
	}
	// process video
	int64_t pts = (int64_t)(time_since_start * ctx->framerate);
	if (pts >= ctx->next_video_pts) {
		if (camera_copy_to_vpx_image(camera, &ctx->vpx_image)) {
			write_video_frame(ctx, &ctx->vpx_image, pts);
		}
		ctx->next_video_pts = pts + 1;
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
		write_video_frame(ctx, NULL, -1);
		// finish video stream
		ogg_packet oggp = {
			.packet = NULL,
			.bytes = 0,
			.granulepos = vp8_pts_to_gp(ctx->next_video_pts, false),
			.b_o_s = false,
			.packetno = ctx->video_packetno++,
			.e_o_s = true,
		};
		write_packet_to_stream(ctx, &ctx->video_stream, &oggp);
		// flush audio encoder
		write_audio_frame(ctx, 0);
	}
	if (ctx->outfile) {
		fclose(ctx->outfile);
		ctx->outfile = NULL;
	}
	if (ctx->vpx.iface) {
		vpx_codec_destroy(&ctx->vpx);
		ctx->vpx.iface = NULL;
	}
	if (ctx->vpx_image.planes[0]) {
		vpx_img_free(&ctx->vpx_image);
		ctx->vpx_image.planes[0] = NULL;
	}
	vorbis_dsp_clear(&ctx->vorbis);
	vorbis_info_clear(&ctx->vorbis_info);
	vorbis_block_clear(&ctx->vorbis_block);
	ogg_stream_clear(&ctx->video_stream);
	ogg_stream_clear(&ctx->audio_stream);
}

void video_quit(VideoContext *ctx) {
	if (!ctx) return;
	video_stop(ctx);
	if (ctx->audio_device) {
		SDL_CloseAudioDevice(ctx->audio_device);
	}
	free(ctx);
}

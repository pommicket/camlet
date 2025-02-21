#include "camera.h"
#include <linux/videodev2.h>
#include <sodium.h>
#include <libv4l2.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <tgmath.h>
#include "ds.h"
#include "3rd_party/stb_image_write.h"
#include <jpeglib.h>

#define CAMERA_MAX_BUFFERS 4
struct Camera {
	char *dev_path;
	char *name;
	uint32_t input_idx;
	struct v4l2_format curr_format;
	crypto_generichash_state hash_state;
	int fd;
	Hash hash;
	uint8_t *read_frame;
	// number of bytes actually read into current frame.
	// this can be variable for compressed formats, and doesn't match v4l2_format sizeimage for grayscale for example
	size_t frame_bytes_set;
	bool any_frames;
	int curr_frame_idx;
	int buffer_count;
	struct v4l2_buffer frame_buffer;
	CameraAccessMethod access_method;
	PictureFormat best_format;
	PictureFormat *formats;
	size_t mmap_size[CAMERA_MAX_BUFFERS];
	uint8_t *mmap_frames[CAMERA_MAX_BUFFERS];
	uint8_t *userp_frames[CAMERA_MAX_BUFFERS];
	// buffer used for jpeg decompression and format conversion for saving images
	// should always be big enough for a 8bpc RGB frame
	uint8_t *decompression_buf;
};

static GlProcs gl;

void camera_init(const GlProcs *procs) {
	gl = *procs;
}

static int uint32_cmp_qsort(const void *av, const void *bv) {
	uint32_t a = *(const uint32_t *)av, b = *(const uint32_t *)bv;
	if (a < b) return -1;
	if (a > b) return 1;
	return 0;
}

int picture_format_cmp_resolution(const PictureFormat *a, const PictureFormat *b) {
	if (a->width < b->width) return -1;
	if (a->width > b->width) return 1;
	if (a->height < b->height) return -1;
	if (a->height > b->height) return 1;
	return 0;
}

int picture_format_cmp_qsort(const void *av, const void *bv) {
	const PictureFormat *a = av, *b = bv;
	if (a->pixfmt < b->pixfmt) return -1;
	if (a->pixfmt > b->pixfmt) return 1;
	int cmp = picture_format_cmp_resolution(a, b);
	if (cmp) return cmp;
	return 0;
}

const char *pixfmt_to_string(uint32_t pixfmt) {
	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB332: return "RGB332";
	case V4L2_PIX_FMT_RGB444: return "RGB444";
	case V4L2_PIX_FMT_XRGB444: return "4bpc XRGB";
	case V4L2_PIX_FMT_RGBX444: return "4bpc RGBX";
	case V4L2_PIX_FMT_XBGR444: return "4bpc XBGR";
	case V4L2_PIX_FMT_BGRX444: return "4bpc BGRX";
	case V4L2_PIX_FMT_RGB555:  return "RGB555";
	case V4L2_PIX_FMT_XRGB555: return "XRGB555";
	case V4L2_PIX_FMT_RGBX555: return "RGBX555";
	case V4L2_PIX_FMT_XBGR555: return "XBGR555";
	case V4L2_PIX_FMT_BGRX555: return "BGRX555";
	case V4L2_PIX_FMT_RGB565:  return "RGB565";
	case V4L2_PIX_FMT_RGB555X: return "RGB555BE";
	case V4L2_PIX_FMT_XRGB555X: return "XRGB555BE";
	case V4L2_PIX_FMT_RGB565X: return "RGB565BE";
	case V4L2_PIX_FMT_BGR24: return "8bpc BGR";
	case V4L2_PIX_FMT_RGB24: return "8bpc RGB";
	case V4L2_PIX_FMT_XBGR32: return "8bpc XBGR";
	case V4L2_PIX_FMT_BGRX32: return "8bpc BGRX";
	case V4L2_PIX_FMT_RGBX32: return "8bpc RGBX";
	case V4L2_PIX_FMT_XRGB32: return "8bpc XRGB";
	case V4L2_PIX_FMT_GREY: return "8-bit grayscale";
	case V4L2_PIX_FMT_Y4: return "4-bit grayscale";
	case V4L2_PIX_FMT_YUYV: return "YUYV 4:2:2";
	case V4L2_PIX_FMT_YYUV: return "YYUV 4:2:2";
	case V4L2_PIX_FMT_YVYU: return "YVYU 4:2:2";
	case V4L2_PIX_FMT_UYVY: return "UYVY 4:2:2";
	case V4L2_PIX_FMT_VYUY: return "VYUY 4:2:2";
	case V4L2_PIX_FMT_YUV444: return "4bpc YUV";
	case V4L2_PIX_FMT_YUV555: return "5bpc YUV";
	case V4L2_PIX_FMT_YUV565: return "YUV565";
	case V4L2_PIX_FMT_YUV24: return "8bpc YUV";
	case V4L2_PIX_FMT_XYUV32: return "8bpc XYUV";
	case V4L2_PIX_FMT_VUYX32: return "8bpc VUYX";
	case V4L2_PIX_FMT_YUVX32: return "8bpc YUVX";
	case V4L2_PIX_FMT_MJPEG:  return "MJPEG";
	case V4L2_PIX_FMT_JPEG: return "JPEG";
	case V4L2_PIX_FMT_MPEG: return "MPEG";
	case V4L2_PIX_FMT_H264: return "H264";
	case V4L2_PIX_FMT_H264_NO_SC: return "AVC1";
	case V4L2_PIX_FMT_H264_MVC: return "H264 MVC";
	case V4L2_PIX_FMT_H263: return "H263";
	case V4L2_PIX_FMT_MPEG1: return "MPEG1";
	case V4L2_PIX_FMT_MPEG2: return "MPEG2";
	case V4L2_PIX_FMT_MPEG4: return "MPEG4";
	case V4L2_PIX_FMT_XVID: return "XVID";
	case V4L2_PIX_FMT_NV12: return "Y/CbCr 4:2:0";
	case V4L2_PIX_FMT_NV21: return "Y/CrCb 4:2:0";
	case V4L2_PIX_FMT_NV16: return "Y/CbCr 4:2:2";
	case V4L2_PIX_FMT_NV61: return "Y/CrCb 4:2:2";
	case V4L2_PIX_FMT_NV24: return "Y/CbCr 4:4:4";
	case V4L2_PIX_FMT_NV42: return "Y/CrCb 4:4:4";
	case V4L2_PIX_FMT_YUV410: return "Y/Cb/Cr 4:1:0";
	case V4L2_PIX_FMT_YVU410: return "Y/Cr/Cb 4:1:0";
	case V4L2_PIX_FMT_YUV411P: return "Y/Cb/Cr 4:1:1";
	case V4L2_PIX_FMT_YUV420: return "Y/Cb/Cr 4:2:0";
	case V4L2_PIX_FMT_YVU420: return "Y/Cr/Cb 4:2:0";
	case V4L2_PIX_FMT_YUV422P: return "Y/Cb/Cr 4:2:2";
	default: {
		static char s[5];
		memcpy(s, &pixfmt, 4);
		return s;
		}
	}
}

bool pix_fmt_supported(uint32_t pixfmt) {
	switch (pixfmt) {
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_BGR24:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_GREY:
	case V4L2_PIX_FMT_MJPEG:
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_YVU420:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		return true;
	}
	return false;
}

static bool camera_setup_with_read(Camera *camera) {
	camera->access_method = CAMERA_ACCESS_READ;
	uint32_t image_size = camera->curr_format.fmt.pix.sizeimage;
	camera->read_frame = realloc(camera->read_frame, image_size);
	if (!camera->read_frame) {
		perror("realloc");
		return false;
	}
	memset(camera->read_frame, 0, image_size);
	return camera->read_frame != NULL;
}
static bool camera_setup_with_mmap(Camera *camera) {
	camera->access_method = CAMERA_ACCESS_MMAP;
	struct v4l2_requestbuffers req = {0};
	req.count = CAMERA_MAX_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (v4l2_ioctl(camera->fd, VIDIOC_REQBUFS, &req) != 0) {
		perror("v4l2_ioctl VIDIOC_REQBUFS");
		return false;
	}
	camera->buffer_count = req.count;
	for (int i = 0; i < camera->buffer_count; i++) {
		struct v4l2_buffer buf = {0};
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (v4l2_ioctl(camera->fd, VIDIOC_QUERYBUF, &buf) != 0) {
			perror("v4l2_ioctl VIDIOC_QUERYBUF");
			return false;
		}
		camera->mmap_size[i] = buf.length;
		camera->mmap_frames[i] = v4l2_mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
			MAP_SHARED, camera->fd, buf.m.offset);
		if (camera->mmap_frames[i] == MAP_FAILED) {
			camera->mmap_frames[i] = NULL;
			perror("mmap");
			return false;
		}
	}
	for (int i = 0; i < camera->buffer_count; i++) {
		struct v4l2_buffer buf = {0};
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (v4l2_ioctl(camera->fd, VIDIOC_QBUF, &buf) != 0) {
			perror("v4l2_ioctl VIDIOC_QBUF");
			return false;
		}
	}
	if (v4l2_ioctl(camera->fd,
		VIDIOC_STREAMON,
		(enum v4l2_buf_type[1]) { V4L2_BUF_TYPE_VIDEO_CAPTURE }) != 0) {
		perror("v4l2_ioctl VIDIOC_STREAMON");
		return false;
	}
	return true;
}

PictureFormat *camera_get_resolutions_with_pixfmt(Camera *camera, uint32_t pixfmt) {
	PictureFormat *available = NULL;
	arr_foreach_ptr(camera->formats, PictureFormat, fmt) {
		if (fmt->pixfmt == pixfmt) {
			arr_add(available, *fmt);
		}
	}
	return available;
}
uint32_t *camera_get_pixfmts(Camera *camera) {
	uint32_t *available = NULL;
	arr_add(available, V4L2_PIX_FMT_RGB24);
	arr_foreach_ptr(camera->formats, const PictureFormat, fmt) {
		if (!pix_fmt_supported(fmt->pixfmt))
			continue;
		arr_foreach_ptr(available, uint32_t, prev) {
			if (*prev == fmt->pixfmt) goto skip;
		}
		arr_add(available, fmt->pixfmt);
		skip:;
	}
	arr_qsort(available, uint32_cmp_qsort);
	return available;
}
PictureFormat camera_closest_resolution(Camera *camera, uint32_t pixfmt, int32_t desired_width, int32_t desired_height) {
	PictureFormat best_format = {.pixfmt = pixfmt};
	int32_t best_score = INT32_MIN;
	arr_foreach_ptr(camera->formats, const PictureFormat, fmt) {
		if (fmt->pixfmt != pixfmt) {
			continue;
		}
		int32_t score = -abs(fmt->width - desired_width) + abs(fmt->height - desired_height);
		if (score >= best_score) {
			best_score = score;
			best_format = *fmt;
		}
	}
	return best_format;
}

static bool camera_setup_with_userp(Camera *camera) {
	camera->access_method = CAMERA_ACCESS_USERP;
	return false;
/*
TODO: test me with a camera that supports userptr i/o
	struct v4l2_requestbuffers req = {0};
	req.count = CAMERA_MAX_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;
	if (v4l2_ioctl(camera->fd, VIDIOC_REQBUFS, &req) != 0) {
		perror("v4l2_ioctl VIDIOC_REQBUFS");
		return false;
	}
	for (int i = 0; i < CAMERA_MAX_BUFFERS; i++) {
		camera->userp_frames[i] = calloc(1, camera->curr_format.fmt.pix.sizeimage);
		struct v4l2_buffer buf = {0};
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR;
		buf.index = i;
		buf.m.userptr = (unsigned long)camera->userp_frames[i];
		buf.length = camera->curr_format.fmt.pix.sizeimage;
		if (v4l2_ioctl(camera->fd, VIDIOC_QBUF, &buf) != 0) {
			perror("v4l2_ioctl VIDIOC_QBUF");
		}
	}
	if (v4l2_ioctl(camera->fd,
		VIDIOC_STREAMON,
		(enum v4l2_buf_type[1]) { V4L2_BUF_TYPE_VIDEO_CAPTURE }) != 0) {
		perror("v4l2_ioctl VIDIOC_STREAMON");
		return false;
	}
	return true;*/
}
static bool camera_stop_io(Camera *camera) {
	// Just doing VIDIOC_STREAMOFF doesn't seem to be enough to prevent EBUSY.
	//  (Even if we dequeue all buffers afterwards)
	// Re-opening doesn't seem to be necessary for read-based access for me,
	// but idk if that's true on all cameras.
	v4l2_close(camera->fd);
	camera->fd = v4l2_open(camera->dev_path, O_RDWR);
	if (camera->fd < 0) {
		perror("v4l2_open");
		return false;
	}
	return true;
}
int32_t camera_frame_width(Camera *camera) {
	return camera->curr_format.fmt.pix.width;
}
int32_t camera_frame_height(Camera *camera) {
	return camera->curr_format.fmt.pix.height;
}
PictureFormat camera_picture_format(Camera *camera) {
	return (PictureFormat) {
		.width = camera_frame_width(camera),
		.height = camera_frame_height(camera),
		.pixfmt = camera->curr_format.fmt.pix.pixelformat
	};
}

static uint8_t *camera_curr_frame(Camera *camera) {
	if (!camera->any_frames)
		return NULL;
	if (camera->read_frame)
		return camera->read_frame;
	if (camera->mmap_frames[camera->curr_frame_idx])
		return camera->mmap_frames[camera->curr_frame_idx];
	assert(camera->userp_frames[camera->curr_frame_idx]);
	return camera->userp_frames[camera->curr_frame_idx];
}

static float clampf(float x, float min, float max) {
	if (x < min) return min;
	if (x > max) return max;
	return x;
}

// SEE ALSO: identically named function in fragment shader
static void ycbcr_ITU_R_601_to_rgb(float y, float cb, float cr, float rgb[3]) {
	rgb[0] = clampf(powf(y + 1.596f * cr - 0.864f, 0.9f), 0, 1);
	rgb[1] = clampf(powf(1.164f * y - 0.378f * cb - 0.813f * cr + 0.525f, 1.1f), 0, 1);
	rgb[2] = clampf(powf(1.164f * y + 2.107f * cb - 1.086f, 1.3f), 0, 1);
}

static uint8_t *curr_frame_rgb24(Camera *camera) {
	uint8_t *curr_frame = camera_curr_frame(camera);
	if (!curr_frame || !camera->decompression_buf)
		return NULL;
	if (camera_pixel_format(camera) == V4L2_PIX_FMT_RGB24) {
		return curr_frame;
	}
	if (camera_pixel_format(camera) == V4L2_PIX_FMT_MJPEG) {
		return camera->decompression_buf;
	}
	int32_t frame_width = camera_frame_width(camera);
	int32_t frame_height = camera_frame_height(camera);
	const uint8_t *in = curr_frame, *in_y = NULL, *in_cb = NULL, *in_cr = NULL, *in_cbcr = NULL;
	uint8_t *out = camera->decompression_buf;
	uint32_t pixfmt = camera_pixel_format(camera);
	switch (pixfmt) {
	case V4L2_PIX_FMT_BGR24: {
		for (int32_t y = 0; y < frame_height; y++) {
			for (int32_t x = 0; x < frame_width; x++) {
				*out++ = in[2];
				*out++ = in[1];
				*out++ = in[0];
				in += 3;
			}
		}
		return camera->decompression_buf;
		} break;
	case V4L2_PIX_FMT_GREY: {
		for (int32_t y = 0; y < frame_height; y++) {
			for (int32_t x = 0; x < frame_width; x++) {
				uint8_t b = *in++;
				*out++ = b;
				*out++ = b;
				*out++ = b;
			}
		}
		return camera->decompression_buf;
		} break;
	case V4L2_PIX_FMT_YUYV: {
		for (int32_t y = 0; y < frame_height; y++) {
			for (int32_t x = 0; x < frame_width / 2; x++) {
				float y0 = (float)(*in++) * (1.0f / 255.0f);
				float cb = (float)(*in++) * (1.0f / 255.0f);
				float y1 = (float)(*in++) * (1.0f / 255.0f);
				float cr = (float)(*in++) * (1.0f / 255.0f);
				float rgb0[3], rgb1[3];
				ycbcr_ITU_R_601_to_rgb(y0, cb, cr, rgb0);
				ycbcr_ITU_R_601_to_rgb(y1, cb, cr, rgb1);
				*out++ = (uint8_t)roundf(rgb0[0] * 255);
				*out++ = (uint8_t)roundf(rgb0[1] * 255);
				*out++ = (uint8_t)roundf(rgb0[2] * 255);
				*out++ = (uint8_t)roundf(rgb1[0] * 255);
				*out++ = (uint8_t)roundf(rgb1[1] * 255);
				*out++ = (uint8_t)roundf(rgb1[2] * 255);
			}
		}
		return camera->decompression_buf;
		}
	case V4L2_PIX_FMT_YUV420:
		in_y = curr_frame;
		in_cb = curr_frame + (size_t)frame_width * (size_t)frame_height;
		in_cr = curr_frame + (size_t)frame_width * (size_t)frame_height * 5 / 4;
		goto yuv420_planar;
	case V4L2_PIX_FMT_YVU420:
		in_y = curr_frame;
		in_cr = curr_frame + (size_t)frame_width * (size_t)frame_height;
		in_cb = curr_frame + (size_t)frame_width * (size_t)frame_height * 5 / 4;
		goto yuv420_planar;
	yuv420_planar:
		for (int32_t row = 0; row < frame_height; row++) {
			for (int32_t col = 0; col < frame_width; col++) {
				float y = (float)(*in_y++) * (1.0f / 255.0f);
				float cb = (float)(*in_cb) * (1.0f / 255.0f);
				float cr = (float)(*in_cr) * (1.0f / 255.0f);
				if (col % 2 == 1) {
					in_cb++;
					in_cr++;
				}
				float rgb[3];
				ycbcr_ITU_R_601_to_rgb(y, cb, cr, rgb);
				*out++ = (uint8_t)roundf(rgb[0] * 255);
				*out++ = (uint8_t)roundf(rgb[1] * 255);
				*out++ = (uint8_t)roundf(rgb[2] * 255);
			}
			if (row % 2 == 0) {
				// go back to start of cb, cr row
				in_cb -= frame_width / 2;
				in_cr -= frame_width / 2;
			}
		}
		return camera->decompression_buf;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		in_y = curr_frame;
		in_cbcr = curr_frame + (size_t)frame_width * (size_t)frame_height;
		for (int32_t row = 0; row < frame_height; row++) {
			for (int32_t col = 0; col < frame_width; col++) {
				float y = (float)(*in_y++) * (1.0f / 255.0f);
				float cb = (float)(in_cbcr[pixfmt == V4L2_PIX_FMT_NV21]) * (1.0f / 255.0f);
				float cr = (float)(in_cbcr[pixfmt == V4L2_PIX_FMT_NV12]) * (1.0f / 255.0f);
				if (col % 2 == 1) {
					in_cbcr += 2;
				}
				float rgb[3];
				ycbcr_ITU_R_601_to_rgb(y, cb, cr, rgb);
				*out++ = (uint8_t)roundf(rgb[0] * 255);
				*out++ = (uint8_t)roundf(rgb[1] * 255);
				*out++ = (uint8_t)roundf(rgb[2] * 255);
			}
			if (row % 2 == 0) {
				// go back to start of cbcr row
				in_cbcr -= frame_width;
			}
		}
		return camera->decompression_buf;
	}
	assert(false);
	return NULL;
}

bool camera_save_jpg(Camera *camera, const char *name, int quality) {
	if (camera_pixel_format(camera) == V4L2_PIX_FMT_MJPEG && camera_curr_frame(camera)) {
		// frame is already in jpeg format
		FILE *fp = fopen(name, "wb");
		if (!fp) {
			perror("fopen");
			return false;
		}
		fwrite(camera_curr_frame(camera), 1, camera->frame_bytes_set, fp);
		fclose(fp);
	}
	uint8_t *frame = curr_frame_rgb24(camera);
	if (frame) {
		uint32_t frame_width = camera_frame_width(camera);
		uint32_t frame_height = camera_frame_height(camera);
		bool ret = stbi_write_jpg(name, frame_width, frame_height, 3, frame, quality) != 0;
		return ret;
	} else {
		return false;
	}
}
bool camera_save_png(Camera *camera, const char *name) {
	uint8_t *frame = curr_frame_rgb24(camera);
	if (frame) {
		uint32_t frame_width = camera_frame_width(camera);
		uint32_t frame_height = camera_frame_height(camera);
		return stbi_write_png(name, frame_width, frame_height, 3, frame, 0) != 0;
	} else {
		return false;
	}
}

typedef struct {
	struct jpeg_error_mgr base;
	bool error;
} JpegErrorMessenger;
static void jpeg_error_handler(j_common_ptr cinfo) {
	((JpegErrorMessenger *)cinfo->err)->error = true;
	cinfo->err->output_message(cinfo);
}

bool camera_next_frame(Camera *camera) {
	struct pollfd pollfd = {.fd = camera->fd, .events = POLLIN};
	// check whether there is any data available from camera
	// NOTE: O_NONBLOCK on v4l2_camera doesn't seem to work, at least on my camera
	if (poll(&pollfd, 1, 1) <= 0) {
		return false;
	}
	switch (camera->access_method) {
	uint32_t memory;
	case CAMERA_ACCESS_NOT_SETUP:
		return false;
	case CAMERA_ACCESS_READ:
		camera->frame_bytes_set = v4l2_read(camera->fd, camera->read_frame, camera->curr_format.fmt.pix.sizeimage);
		camera->any_frames = true;
		break;
	case CAMERA_ACCESS_MMAP:
		memory = V4L2_MEMORY_MMAP;
		goto buf;
	case CAMERA_ACCESS_USERP:
		memory = V4L2_MEMORY_USERPTR;
		goto buf;
	buf: {
		if (camera->frame_buffer.type) {
			// queue back in previous buffer
			v4l2_ioctl(camera->fd, VIDIOC_QBUF, &camera->frame_buffer);
			camera->frame_buffer.type = 0;
		}
		struct v4l2_buffer buf = {0};
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = memory;
		if (v4l2_ioctl(camera->fd, VIDIOC_DQBUF, &buf) != 0) {
			static bool printed_error;
			if (!printed_error) {
				perror("v4l2_ioctl VIDIOC_DQBUF");
				printed_error = true;
			}
			return false;
		}
		camera->frame_bytes_set = buf.bytesused;
		camera->curr_frame_idx = buf.index;
		camera->frame_buffer = buf;
		camera->any_frames = true;
		break;
		}
	default:
		#if DEBUG
		assert(false);
		#endif
		return false;
	}
	const uint8_t *curr_frame = camera_curr_frame(camera);
	const int32_t frame_width = camera_frame_width(camera);
	const int32_t frame_height = camera_frame_height(camera);
	if (camera_pixel_format(camera) == V4L2_PIX_FMT_MJPEG) {
		// decompress the jpeg
		// NOTE: libjpeg is ~2x as fast as stb_image
		JpegErrorMessenger messenger = {.base = {.error_exit = jpeg_error_handler}};
		struct jpeg_decompress_struct cinfo = {0};
		cinfo.err = jpeg_std_error(&messenger.base);
		jpeg_create_decompress(&cinfo);
		if (!messenger.error)
			jpeg_mem_src(&cinfo, curr_frame, camera->frame_bytes_set);
		if (!messenger.error)
			jpeg_read_header(&cinfo, true);
		if (!messenger.error)
			jpeg_start_decompress(&cinfo);
		if (!messenger.error && cinfo.output_components != 3) {
			fprintf(stderr, "JPEG has %d components, instead of 3. That's messed up.\n",
				cinfo.output_components);
			messenger.error = true;
		}
		if (!messenger.error && (int32_t)cinfo.output_width != frame_width) {
			fprintf(stderr, "JPEG from camera has width %" PRId32 ", but I was expecting %" PRId32 "\n",
				(int32_t)cinfo.output_width, frame_width);
		}
		if (!messenger.error && (int32_t)cinfo.output_height != frame_height) {
			fprintf(stderr, "JPEG from camera has height %" PRId32 ", but I was expecting %" PRId32 "\n",
				(int32_t)cinfo.output_height, frame_height);
		}
		if (!messenger.error) {
			for (int32_t y = 0; y < frame_height; y++) {
				jpeg_read_scanlines(&cinfo, (uint8_t*[1]){ &camera->decompression_buf[(size_t)y*frame_width*3] }, 1);
			}
		}
		if (!messenger.error)
			jpeg_finish_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		if (messenger.error) {
			camera->any_frames = false;
			return false;
		}
	}
	return true;
}

void camera_update_gl_textures(Camera *camera, const GLuint textures[3]) {
	int prev_align = 1;
	gl.BindTexture(GL_TEXTURE_2D, textures[0]);
	gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
	int32_t frame_width = camera_frame_width(camera), frame_height = camera_frame_height(camera);
	for (int align = 8; align >= 1; align >>= 1) {
		if (frame_width % align == 0) {
			gl.PixelStorei(GL_UNPACK_ALIGNMENT, align);
			break;
		}
	}
	uint8_t *curr_frame = camera_curr_frame(camera);
	if (curr_frame) {
		switch (camera->curr_format.fmt.pix.pixelformat) {
		case V4L2_PIX_FMT_RGB24:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height * 3)
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame_width, frame_height, 0, GL_RGB, GL_UNSIGNED_BYTE, curr_frame);
			break;
		case V4L2_PIX_FMT_BGR24:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height * 3)
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame_width, frame_height, 0, GL_BGR, GL_UNSIGNED_BYTE, curr_frame);
			break;
		case V4L2_PIX_FMT_GREY:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height)
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width, frame_height, 0, GL_RED, GL_UNSIGNED_BYTE, curr_frame);
			break;
		case V4L2_PIX_FMT_YUYV:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height * 2)
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame_width / 2, frame_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, curr_frame);
			break;
		case V4L2_PIX_FMT_YUV420:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height * 3 / 2) {
				// Y plane
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width, frame_height, 0, GL_RED, GL_UNSIGNED_BYTE, curr_frame);
				// Cb plane
				gl.BindTexture(GL_TEXTURE_2D, textures[1]);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width / 2, frame_height / 2, 0, GL_RED, GL_UNSIGNED_BYTE,
					curr_frame + (size_t)frame_width * (size_t)frame_height);
				// Cr plane
				gl.BindTexture(GL_TEXTURE_2D, textures[2]);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width / 2, frame_height / 2, 0, GL_RED, GL_UNSIGNED_BYTE,
					curr_frame + (size_t)frame_width * (size_t)frame_height * 5 / 4);
			}
			break;
		case V4L2_PIX_FMT_YVU420:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height * 3 / 2) {
				// same as above, but swap textures[1] and textures[2] so that we only have to handle one case in the shader
				// Y plane
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width, frame_height, 0, GL_RED, GL_UNSIGNED_BYTE, curr_frame);
				// Cr plane
				gl.BindTexture(GL_TEXTURE_2D, textures[2]);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width / 2, frame_height / 2, 0, GL_RED, GL_UNSIGNED_BYTE,
					curr_frame + (size_t)frame_width * (size_t)frame_height);
				// Cb plane
				gl.BindTexture(GL_TEXTURE_2D, textures[1]);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width / 2, frame_height / 2, 0, GL_RED, GL_UNSIGNED_BYTE,
					curr_frame + (size_t)frame_width * (size_t)frame_height * 5 / 4);
			}
			break;
		case V4L2_PIX_FMT_NV12:
		case V4L2_PIX_FMT_NV21:
			if (camera->frame_bytes_set >= (size_t)frame_width * (size_t)frame_height * 3 / 2) {
				// Y plane
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_width, frame_height, 0, GL_RED, GL_UNSIGNED_BYTE, curr_frame);
				// CbCr or CrCb plane
				gl.BindTexture(GL_TEXTURE_2D, textures[1]);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG, frame_width / 2, frame_height / 2, 0, GL_RG, GL_UNSIGNED_BYTE,
					curr_frame + (size_t)frame_width * (size_t)frame_height);
			}
			break;
		case V4L2_PIX_FMT_MJPEG: {
			// "motion jpeg" is actually just a series of jpegs
			gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame_width, frame_height, 0, GL_RGB, GL_UNSIGNED_BYTE,
				camera->decompression_buf);
			} break;
		}
	}
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}

const char *camera_name(Camera *camera) {
	return camera->name;
}

uint32_t camera_pixel_format(Camera *camera) {
	return camera->curr_format.fmt.pix.pixelformat;
}

CameraAccessMethod camera_access_method(Camera *camera) {
	return camera->access_method;
}

void camera_close(Camera *camera) {
	free(camera->read_frame);
	camera->read_frame = NULL;
	free(camera->decompression_buf);
	camera->decompression_buf = NULL;
	for (int i = 0; i < CAMERA_MAX_BUFFERS; i++) {
		if (camera->mmap_frames[i]) {
			v4l2_munmap(camera->mmap_frames[i], camera->mmap_size[i]);
			camera->mmap_frames[i] = NULL;
		}
		free(camera->userp_frames[i]);
		camera->userp_frames[i] = NULL;
	}
	if (camera->fd >= 0)
		v4l2_close(camera->fd);
}

void camera_free(Camera *camera) {
	camera_close(camera);
	free(camera);
}

bool camera_set_format(Camera *camera, PictureFormat picfmt, CameraAccessMethod access, bool force) {
	if (!force
		&& camera->access_method == access
		&& picture_format_cmp_qsort((PictureFormat[1]) { camera_picture_format(camera) }, &picfmt) == 0) {
		// no changes needed
		return true;
	}
	camera->any_frames = false;
	camera->access_method = access;
	for (int i = 0; i < camera->buffer_count; i++) {
		if (camera->mmap_frames[i]) {
			v4l2_munmap(camera->mmap_frames[i], camera->mmap_size[i]);
			camera->mmap_frames[i] = NULL;
		}
	}
	free(camera->read_frame);
	camera->read_frame = NULL;
	struct v4l2_format format = {0};
	camera_stop_io(camera); // prevent EBUSY when changing format
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.field = V4L2_FIELD_ANY;
	// v4l2 should be able to output rgb24 for all reasonable cameras
	uint32_t pixfmt = V4L2_PIX_FMT_RGB24;
	if (pix_fmt_supported(picfmt.pixfmt)) {
		// we can handle this format actually
		pixfmt = picfmt.pixfmt;
	}
	format.fmt.pix.pixelformat = pixfmt;
	format.fmt.pix.width = picfmt.width;
	format.fmt.pix.height = picfmt.height;
	camera->decompression_buf = realloc(camera->decompression_buf, (size_t)3 * picfmt.width * picfmt.height);
	if (!camera->decompression_buf) {
		perror("realloc");
		return false;
	}
	if (v4l2_ioctl(camera->fd, VIDIOC_S_FMT, &format) != 0) {
		perror("v4l2_ioctl VIDIOC_S_FMT");
		return false;
	}
	camera->curr_format = format;
	//printf("image size = %uB\n",format.fmt.pix.sizeimage);
	switch (camera->access_method) {
	case CAMERA_ACCESS_READ:
		return camera_setup_with_read(camera);
	case CAMERA_ACCESS_MMAP:
		return camera_setup_with_mmap(camera);
	case CAMERA_ACCESS_USERP:
		return camera_setup_with_userp(camera);
	default:
		#if DEBUG
		assert(false);
		#endif
		return false;
	}
}

bool camera_open(Camera *camera) {
	if (!camera->access_method)
		camera->access_method = CAMERA_ACCESS_MMAP;
	// camera should not already be open
	assert(!camera->read_frame);
	assert(!camera->mmap_frames[0]);
	assert(!camera->userp_frames[0]);
	camera->fd = v4l2_open(camera->dev_path, O_RDWR);
	if (camera->fd < 0) {
		perror("v4l2_open");
		return false;
	}
	if (v4l2_ioctl(camera->fd, VIDIOC_S_INPUT, &camera->input_idx) != 0) {
		perror("v4l2_ioctl");
		return false;
	}
	camera_set_format(camera, camera->best_format, camera->access_method, true);
	return true;
}


static void cameras_from_device_with_fd(const char *dev_path, const char *serial, int fd, Camera ***cameras) {
	struct v4l2_capability cap = {0};
	v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return;
	struct v4l2_input input = {0};
	for (uint32_t input_idx = 0; ; input_idx++) {
		input.index = input_idx;
		if (v4l2_ioctl(fd, VIDIOC_ENUMINPUT, &input) == -1) break;
		if (input.type != V4L2_INPUT_TYPE_CAMERA) continue;
		Camera *camera = calloc(1, sizeof *camera);
		if (!camera) {
			perror("calloc");
			return;
		}
		camera->fd = -1;
		crypto_generichash_init(&camera->hash_state, NULL, 0, HASH_SIZE);
		crypto_generichash_update(&camera->hash_state, cap.card, strlen((const char *)cap.card) + 1);
		crypto_generichash_update(&camera->hash_state, input.name, strlen((const char *)input.name) + 1);
		struct v4l2_fmtdesc fmtdesc = {0};
		printf("-----\n");
		for (uint32_t fmt_idx = 0; ; fmt_idx++) {
			fmtdesc.index = fmt_idx;
			fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == -1) break;
			uint32_t fourcc[2] = {fmtdesc.pixelformat, 0};
			printf("  - %s (%s)\n",fmtdesc.description, (const char *)fourcc);
			struct v4l2_frmsizeenum frmsize = {0};
			if (serial && *serial)
				crypto_generichash_update(&camera->hash_state, (const uint8_t *)serial, strlen(serial) + 1);
			for (uint32_t frmsz_idx = 0; ; frmsz_idx++) {
				frmsize.index = frmsz_idx;
				frmsize.pixel_format = fmtdesc.pixelformat;
				if (v4l2_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == -1) break;
				// are there even any stepwise cameras out there?? who knows.
				uint32_t frame_width = frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ? frmsize.discrete.width : frmsize.stepwise.max_width;
				uint32_t frame_height = frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ? frmsize.discrete.height : frmsize.stepwise.max_height;
				if (frame_width % 4 || frame_height % 4) {
					// fucked up frame size
					// (would probably break some YUV pixel formats)
					continue;
				}
				arr_add(camera->formats, ((PictureFormat) {
					.width = frame_width,
					.height = frame_height,
					.pixfmt = fmtdesc.pixelformat,
				}));
			}
		}
		if (arr_len(camera->formats) == 0) {
			free(camera);
			continue;
		}
		arr_qsort(camera->formats, picture_format_cmp_qsort);
		// deduplicate
		{
			int i, o;
			for (o = 0, i = 0; i < (int)arr_len(camera->formats); i++) {
				if (i == 0 || picture_format_cmp_qsort(&camera->formats[i-1], &camera->formats[i]) != 0) {
					camera->formats[o++] = camera->formats[i];
				}
			}
			arr_set_len(camera->formats, o);
		}
		camera->input_idx = input_idx;
		camera->dev_path = strdup(dev_path);
		// select best format
		PictureFormat best_format = {0};
		uint32_t desired_format = V4L2_PIX_FMT_RGB24;
		crypto_generichash_update(&camera->hash_state, (const uint8_t *)(const uint32_t [1]){arr_len(camera->formats)}, 4);
		arr_foreach_ptr(camera->formats, PictureFormat, fmt) {
			// Now you might think do we really need this?
			// Is it really not enough to use the device name, input name, and serial number to uniquely identify a camera??
			// No. you fool. Of course there is a Logitech camera with an infrared sensor (for face recognition)
			// that shows up as two video devices with identical names, capabilities, input names, etc. etc.
			// and the only way to distinguish them is the picture formats they support.
			// Oddly Windows doesn't show the infrared camera as an input device.
			// I wonder if there is some way of detecting which one is the "normal" camera.
			// Or perhaps Windows has its own special proprietary driver and we have no way of knowing.
			crypto_generichash_update(&camera->hash_state, (const uint8_t *)&fmt->pixfmt, sizeof fmt->pixfmt);
			crypto_generichash_update(&camera->hash_state, (const uint8_t *)&fmt->width, sizeof fmt->width);
			crypto_generichash_update(&camera->hash_state, (const uint8_t *)&fmt->height, sizeof fmt->height);
			if (best_format.pixfmt == desired_format && fmt->pixfmt != desired_format) {
				continue;
			}
			if ((fmt->pixfmt == desired_format && best_format.pixfmt != desired_format)
				|| fmt->width > best_format.width) {
				best_format = *fmt;
			}
		}
		camera->best_format = best_format;
		camera->name = a_sprintf(
			"%s %s (up to %" PRIu32 "x%" PRIu32 ")", (const char *)cap.card, (const char *)input.name,
			best_format.width, best_format.height
		);
		crypto_generichash_final(&camera->hash_state, camera->hash.hash, sizeof camera->hash.hash);
		arr_add(*cameras, camera);
	}
}

void cameras_from_device(const char *dev_path, const char *serial, Camera ***cameras) {
	int fd = v4l2_open(dev_path, O_RDWR);
	if (fd < 0) {
		perror("v4l2_open");
		return;
	}
	cameras_from_device_with_fd(dev_path, serial, fd, cameras);
	v4l2_close(fd);
}


void camera_update_hash(Camera *camera, const void *data, size_t len) {
	crypto_generichash_update(&camera->hash_state, data, len);
	// should be perfectly fine to copy state?
	crypto_generichash_state state = camera->hash_state;
	crypto_generichash_final(&state, camera->hash.hash, sizeof camera->hash.hash);
}

Hash camera_hash(Camera *camera) {
	return camera->hash;
}

void camera_hash_str(Camera *camera, char str[HASH_SIZE * 2 + 1]) {
	for (int i = 0; i < HASH_SIZE; i++) {
		sprintf(&str[2*i], "%02x", camera->hash.hash[i]);
	}
}

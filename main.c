#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <libv4l2.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include "ds.h"

typedef struct {
	int32_t dev_idx;
	char *name;
} Camera;


static void debug_save_24bpp_bmp(const char *filename, const uint8_t *pixels, uint16_t width, uint16_t height) {
	FILE *fp = fopen(filename, "wb");
	if (!fp) {
		perror("fopen");
		return;
	}
	typedef struct {
		char BM[2];
		char size[4];
		char resv[4];
		char offset[4];
		char hdrsize[4];
		uint16_t width;
		uint16_t height;
		uint16_t planes;
		uint16_t bit_count;
	} BMPHeader;
	BMPHeader hdr = {
		.BM = "BM",
		.width = width,
		.height = height,
		.planes = 1,
		.bit_count = 24,
	};
	uint32_t offset = sizeof(BMPHeader);
	uint32_t file_size = sizeof(BMPHeader) + (uint32_t)width * height * 3;
	memcpy(&hdr.size, &file_size, 4);
	memcpy(&hdr.offset, &offset, 4);
	memcpy(&hdr.hdrsize, (uint32_t[1]) { 12 }, 4);
	fwrite(&hdr, sizeof(BMPHeader), 1, fp);
	for (uint32_t i = 0; i < height; i++) {
		fwrite(pixels + (height-1-i) * width * 3, 3, (size_t)width, fp);
	}
	fclose(fp);
}

char *a_sprintf(PRINTF_FORMAT_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
char *a_sprintf(const char *fmt, ...) {
	// idk if you can always just pass NULL to vsnprintf
	va_list args;
	char fakebuf[2] = {0};
	va_start(args, fmt);
	int ret = vsnprintf(fakebuf, 1, fmt, args);
	va_end(args);
	
	if (ret < 0) return NULL; // bad format or something
	size_t n = (size_t)ret;
	char *str = calloc(1, n + 1);
	va_start(args, fmt);
	vsnprintf(str, n + 1, fmt, args);
	va_end(args);
	return str;
}

void get_cameras_from_device(int32_t dev_idx, int fd, Camera **cameras) {
	struct v4l2_capability cap = {0};
	v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return;
	struct v4l2_input input = {0};
	for (uint32_t input_idx = 0; ; input_idx++) {
		input.index = input_idx;
		if (v4l2_ioctl(fd, VIDIOC_ENUMINPUT, &input) == -1) break;
		if (input.type != V4L2_INPUT_TYPE_CAMERA) continue;
		Camera *camera = arr_addp(*cameras);
		camera->name = a_sprintf(
			"%s %s %s %x %x %x", (const char *)cap.card, (const char *)cap.bus_info, (const char *)input.name,
			cap.device_caps, cap.capabilities, cap.version
		);
		printf("%s %s %s\n", (const char *)cap.card, (const char *)cap.bus_info, (const char *)input.name);
		struct v4l2_fmtdesc fmt = {0};
		for (uint32_t fmt_idx = 0; ; fmt_idx++) {
			fmt.index = fmt_idx;
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == -1) break;
			uint32_t fourcc[2] = {fmt.pixelformat, 0};
			printf("  - %s (%s)\n",fmt.description, (const char *)fourcc);
			struct v4l2_frmsizeenum frmsize = {0};
			for (uint32_t frmsz_idx = 0; ; frmsz_idx++) {
				frmsize.index = frmsz_idx;
				frmsize.pixel_format = fmt.pixelformat;
				if (v4l2_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == -1) break;
				if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
					printf("   - %ux%u\n", frmsize.discrete.width, frmsize.discrete.height);
if (frmsize.discrete.width == 4096 && fmt.pixelformat == V4L2_PIX_FMT_BGR24) {
	uint32_t frame_width = frmsize.discrete.width;
	uint32_t frame_height = frmsize.discrete.height;
	if (v4l2_ioctl(fd, VIDIOC_S_INPUT, &input_idx) == -1) { perror("v4l2_ioctl VIDIOC_S_INPUT"); return; }
	struct v4l2_format format = {0};
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
	format.fmt.pix.field = V4L2_FIELD_ANY;
	format.fmt.pix.bytesperline = 3 * frame_width;
	format.fmt.pix.sizeimage = format.fmt.pix.bytesperline * frame_height;
	format.fmt.pix.width = frame_width;
	format.fmt.pix.height = frame_height;
	if (v4l2_ioctl(fd, VIDIOC_S_FMT, &format) == -1) { perror("v4l2_ioctl VIDIOC_S_FMT"); return; }
	uint8_t *buffer = malloc(format.fmt.pix.sizeimage);
	size_t n = v4l2_read(fd, buffer, format.fmt.pix.sizeimage);
	if (n != format.fmt.pix.sizeimage) {
		perror("read"); return;
	}
	debug_save_24bpp_bmp("out.bmp", buffer, frame_width, frame_height);
	return;
}
				} else {
					printf("   - %ux%u to %ux%u skip %ux%u\n", frmsize.stepwise.min_width, frmsize.stepwise.min_height,
						frmsize.stepwise.max_width, frmsize.stepwise.max_height,
						frmsize.stepwise.step_width, frmsize.stepwise.step_height);
				}
			}
		}
	}
}

int main() {
	if (access("/dev", R_OK) != 0) {
		fprintf(stderr, "Can't access /dev");
		return EXIT_FAILURE;
	}
	Camera *cameras = NULL;
	for (int32_t dev_idx = 0; dev_idx < 100000; dev_idx++) {
		char devpath[32] = {0};
		snprintf(devpath, sizeof devpath, "/dev/video%" PRId32, dev_idx);
		int status = access(devpath, R_OK);
		if (status != 0 && errno == EACCES) {
			// can't read from this device
			continue;
		}
		if (status) break;
		int fd = v4l2_open(devpath, O_RDWR);
		get_cameras_from_device(dev_idx, fd, &cameras);
		v4l2_close(fd);
	}
	printf("Select a camera:\n");
	for (size_t i = 0; i < arr_len(cameras); i++) {
		printf("[%zu] %s\n", i, cameras[i].name);
	}
	return 0;
}

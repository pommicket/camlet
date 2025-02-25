/*
TODO
-save/restore settings
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include <inttypes.h>
#include <errno.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <time.h>
#include <libudev.h>
#include <sodium.h>
#include <fontconfig/fontconfig.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include "util.h"
#include "camera.h"
#include "video.h"

// pixel format used for convenience
#define PIX_FMT_XXXGRAY 0x47585858

static const char *const DEFAULT_OUTPUT_DIR = "~/Pictures/Webcam";

typedef enum {
	MENU_NONE,
	MENU_MAIN,
	MENU_RESOLUTION,
	MENU_INPUT,
	MENU_PIXFMT,
	MENU_HELP,
	MENU_SET_OUTPUT_DIR,
	MENU_FRAMERATE,
	MENU_COUNT
} Menu;

enum {
	MENU_OPT_QUIT = 1,
	MENU_OPT_RESOLUTION,
	MENU_OPT_VIDEO_INPUT,
	MENU_OPT_PIXFMT,
	MENU_OPT_IMGFMT,
	MENU_OPT_SET_OUTPUT_DIR,
	MENU_OPT_TIMER,
	MENU_OPT_FRAMERATE,
};
// use char for MenuOption type so that we can use strlen
typedef char MenuOption;
static const MenuOption main_menu[] = {
	MENU_OPT_VIDEO_INPUT,
	MENU_OPT_RESOLUTION,
	MENU_OPT_TIMER,
	MENU_OPT_IMGFMT,
	MENU_OPT_PIXFMT,
	MENU_OPT_SET_OUTPUT_DIR,
	MENU_OPT_FRAMERATE,
	MENU_OPT_QUIT,
	0
};

typedef enum {
	IMG_FMT_JPEG,
	IMG_FMT_PNG,

	IMG_FMT_COUNT,
} ImageFormat;
static const char *const image_format_names[IMG_FMT_COUNT] = {"JPEG", "PNG"};
static const char *const image_format_extensions[IMG_FMT_COUNT] = {"jpg", "png"};

typedef enum {
	MODE_IMAGE,
	MODE_VIDEO,

	MODE_COUNT,
} CameraMode;

typedef struct {
	Hash *camera_precedence;
	uint32_t pixel_format;
	int timer;
	int32_t image_resolution[2];
	int32_t video_resolution[2];
	int video_framerate;
	int image_framerate;
	ImageFormat image_format;
	char *output_dir;
} Settings;

typedef struct {
	Menu curr_menu;
	int menu_sel[MENU_COUNT];
	bool show_debug;
	bool menu_needs_rerendering;
	bool quit;
	CameraMode mode;
	VideoContext *video;
	double timer_activate_time;
	double flash_time;
	Camera *camera;
	Camera **cameras;
	SDL_Rect *menu_option_rects;
	Settings settings;
} State;

static const int timer_options[] = {0, 2, 5, 10, 15, 30};

#if crypto_generichash_BYTES_MIN > HASH_SIZE
#error "crypto_generichash what happened"
#endif

static GlProcs gl;
static void select_camera(State *state);

static void fatal_error(PRINTF_FORMAT_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
static void fatal_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	static char message[256];
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "camlet error", message, NULL);
	exit(EXIT_FAILURE);
}

#if DEBUG
static void APIENTRY gl_message_callback(GLenum source, GLenum type, unsigned int id, GLenum severity, 
	GLsizei length, const char *message, const void *userParam) {
	(void)source; (void)type; (void)id; (void)length; (void)userParam;
	if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
	printf("Message from OpenGL: %s.\n", message);
}
#endif

static PictureFormat settings_picture_format_for_camera(State *state, Camera *camera) {
	Settings *settings = &state->settings;
	PictureFormat picfmt = {
		.width = state->mode == MODE_VIDEO ? settings->video_resolution[0] : settings->image_resolution[0],
		.height = state->mode == MODE_VIDEO ? settings->video_resolution[1] : settings->image_resolution[1],
		.pixfmt = settings->pixel_format
	};
	if (state->mode == MODE_VIDEO && (!picfmt.width || !picfmt.height)) {
		// default for video
		picfmt.width = 1280;
		picfmt.height = 720;
	}
	if (state->mode == MODE_VIDEO)
		picfmt.pixfmt = V4L2_PIX_FMT_YUV420;
	picfmt = camera_closest_picfmt(camera, picfmt);
	if (state->mode == MODE_VIDEO)
		picfmt.pixfmt = V4L2_PIX_FMT_YUV420;
	return picfmt;
}

static int settings_desired_framerate(State *state) {
	Settings *settings = &state->settings;
	// by default, aim for 30FPS for video (60FPS may be too much to handle)
	int video_framerate = settings->video_framerate ? settings->video_framerate : 30;
	return state->mode == MODE_VIDEO ? video_framerate : settings->image_framerate;
}

// compile a GLSL shader
GLuint gl_compile_shader(char error_buf[256], const char *code, GLenum shader_type) {
	GLuint shader = gl.CreateShader(shader_type);
	char header[128];
	snprintf(header, sizeof header, "#version 130\n\
#line 1\n");
	const char *sources[2] = {
		header,
		code
	};
	gl.ShaderSource(shader, 2, sources, NULL);
	gl.CompileShader(shader);
	GLint status = 0;
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		char log[1024] = {0};
		gl.GetShaderInfoLog(shader, sizeof log - 1, NULL, log);
		if (error_buf) {
			snprintf(error_buf, 256, "Error compiling shader: %s", log);
		} else {
			printf("Error compiling shader: %s\n", log);
		}
		return 0;
	}
	return shader;
}

// link together GL shaders
GLuint gl_link_program(char error_buf[256], GLuint *shaders, size_t count) {
	GLuint program = gl.CreateProgram();
	if (program) {
		for (size_t i = 0; i < count; ++i) {
			if (!shaders[i]) {
				gl.DeleteProgram(program);
				return 0;
			}
			gl.AttachShader(program, shaders[i]);
		}
		gl.LinkProgram(program);
		GLint status = 0;
		gl.GetProgramiv(program, GL_LINK_STATUS, &status);
		if (status == GL_FALSE) {
			char log[1024] = {0};
			gl.GetProgramInfoLog(program, sizeof log - 1, NULL, log);
			if (error_buf) {
				snprintf(error_buf, 256, "Error linking shaders: %s", log);
			} else {
				printf("Error linking shaders: %s\n", log);
			}
			gl.DeleteProgram(program);
			return 0;
		}
	}
	return program;
}

GLuint gl_compile_and_link_shaders(char error_buf[256], const char *vshader_code, const char *fshader_code) {
	GLuint shaders[2];
	shaders[0] = gl_compile_shader(error_buf, vshader_code, GL_VERTEX_SHADER);
	shaders[1] = gl_compile_shader(error_buf, fshader_code, GL_FRAGMENT_SHADER);
	GLuint program = gl_link_program(error_buf, shaders, 2);
	if (shaders[0]) gl.DeleteShader(shaders[0]);
	if (shaders[1]) gl.DeleteShader(shaders[1]);
	if (program && DEBUG) {
		printf("Successfully linked program %u.\n", program);
	}
	return program;
}

static int menu_option_count(State *state) {
	switch (state->curr_menu) {
	case MENU_NONE: return 0;
	case MENU_HELP: return 1;
	case MENU_MAIN: return strlen(main_menu);
	case MENU_INPUT: return (int)arr_len(state->cameras) + 1;
	case MENU_RESOLUTION: {
		PictureFormat *resolutions = camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera));
		int n = (int)arr_len(resolutions) + 1;
		arr_free(resolutions);
		return n;
		}
		break;
	case MENU_PIXFMT: {
		uint32_t *pixfmts = camera_get_pixfmts(state->camera);
		int n = (int)arr_len(pixfmts) + 1;
		arr_free(pixfmts);
		return n;
		}
		break;
	case MENU_FRAMERATE:
		return 1 + popcount64(camera_framerates_supported(state->camera));
	case MENU_SET_OUTPUT_DIR: return 1;
	case MENU_COUNT: break;
	}
	assert(false);
	return 0;
}

static uint32_t sdl_color_to_u32(SDL_Color color) {
	return (uint32_t)color.r << 24 | (uint32_t)color.g << 16 | (uint32_t)color.b << 8 | color.a;
}

static SDL_Rect render_text_to_surface_anchored(TTF_Font *font, SDL_Surface *dest, int x, int y, SDL_Color color, const char *str,
	int xanchor, int yanchor) {
	if (!str[0]) {
		return (SDL_Rect){.x = x, .y = y, .w = 0, .h = TTF_FontLineSkip(font)};
	}
	SDL_Surface *text = TTF_RenderUTF8_Blended(font, str, color);
	x -= (xanchor + 1) * text->w / 2;
	y -= (yanchor + 1) * text->h / 2;
	SDL_BlitSurface(text, NULL, dest, (SDL_Rect[1]){{x, y, 0, 0}});
	int w = text->w, h = text->h;
	SDL_FreeSurface(text);
	return (SDL_Rect){x, y, w, h};
}

static SDL_Rect render_text_to_surface(TTF_Font *font, SDL_Surface *dest, int x, int y, SDL_Color color, const char *str) {
	return render_text_to_surface_anchored(font, dest, x, y, color, str, -1, -1);
}

static void move_to_highest_precedence(Settings *settings, Camera *camera) {
	Hash hash = camera_hash(camera);
	for (size_t i = 0; i < arr_len(settings->camera_precedence); i++) {
		if (hash_eq(settings->camera_precedence[i], hash)) {
			arr_remove(settings->camera_precedence, i);
			break;
		}
	}
	arr_insert(settings->camera_precedence, 0, hash);
}

static void change_timer(State *state, int direction) {
	Settings *settings = &state->settings;
	int k;
	int n_options = (int)SDL_arraysize(timer_options);
	for (k = 0; k < n_options; k++) {
		if (timer_options[k] == settings->timer) {
			break;
		}
	}
	settings->timer = timer_options[((k + direction) % n_options + n_options) % n_options];
	state->menu_needs_rerendering = true;
}

static void menu_select(State *state) {
	Settings *settings = &state->settings;
	if (state->curr_menu == MENU_MAIN) {
		switch (main_menu[state->menu_sel[MENU_MAIN]]) {
		case MENU_OPT_QUIT:
			state->quit = true;
			break;
		case MENU_OPT_RESOLUTION: if (state->camera) {
			state->curr_menu = MENU_RESOLUTION;
			state->menu_needs_rerendering = true;
			// set menu_sel
			PictureFormat *resolutions = camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera));
			state->menu_sel[MENU_RESOLUTION] = 0;
			arr_foreach_ptr(resolutions, PictureFormat, resolution) {
				if (resolution->width == camera_frame_width(state->camera)
					&& resolution->height == camera_frame_height(state->camera)) {
					state->menu_sel[MENU_RESOLUTION] = (int)(resolution - resolutions) + 1;
				}
			}
			arr_free(resolutions);
			} break;
		case MENU_OPT_VIDEO_INPUT:
			if (state->cameras) {
				state->curr_menu = MENU_INPUT;
				state->menu_needs_rerendering = true;
				state->menu_sel[MENU_INPUT] = 0;
				arr_foreach_ptr(state->cameras, Camera *, pcam) {
					if (*pcam == state->camera) {
						state->menu_sel[MENU_INPUT] = (int)(pcam - state->cameras) + 1;
					}
				}
			}
			break;
		case MENU_OPT_PIXFMT: if (state->camera && state->mode != MODE_VIDEO) {
			state->curr_menu = MENU_PIXFMT;
			state->menu_needs_rerendering = true;
			// set menu_sel
			uint32_t *pixfmts = camera_get_pixfmts(state->camera);
			arr_foreach_ptr(pixfmts, uint32_t, pixfmt) {
				if (*pixfmt == camera_pixel_format(state->camera)) {
					state->menu_sel[MENU_PIXFMT] = (int)(pixfmt - pixfmts) + 1;
				}
			}
			arr_free(pixfmts);
			} break;
		case MENU_OPT_IMGFMT: {
			settings->image_format = (settings->image_format + 1) % IMG_FMT_COUNT;
			state->menu_needs_rerendering = true;
			} break;
		case MENU_OPT_SET_OUTPUT_DIR:
			state->curr_menu = MENU_SET_OUTPUT_DIR;
			state->menu_needs_rerendering = true;
			break;
		case MENU_OPT_FRAMERATE: {
			if (!state->camera) break;
			state->curr_menu = MENU_FRAMERATE;
			state->menu_needs_rerendering = true;
			// set menu_sel
			int framerate_idx = 0;
			for (int i = 0; i < 64; i++) {
				if (i == camera_framerate(state->camera)) break;
				if (camera_framerates_supported(state->camera) & ((uint64_t)1 << i))
					framerate_idx++;
			}
			state->menu_sel[MENU_FRAMERATE] = (framerate_idx + 1) % menu_option_count(state);
			} break;
		case MENU_OPT_TIMER:
			change_timer(state, 1);
			break;
		}
	} else if (state->curr_menu == MENU_RESOLUTION) {
		int sel = state->menu_sel[state->curr_menu];
		if (sel == 0) {
			state->curr_menu = MENU_MAIN;
			state->menu_needs_rerendering = true;
			return;
		}
		PictureFormat *resolutions = camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera));
		PictureFormat resolution = resolutions[sel-1];
		if (state->mode == MODE_VIDEO) {
			settings->video_resolution[0] = resolution.width;
			settings->video_resolution[1] = resolution.height;
		} else {
			settings->image_resolution[0] = resolution.width;
			settings->image_resolution[1] = resolution.height;
		}
		camera_set_format(state->camera,
			resolution,
			settings_desired_framerate(state),
			0,
			false);
		arr_free(resolutions);
	} else if (state->curr_menu == MENU_INPUT) {
		int sel = state->menu_sel[state->curr_menu];
		if (sel == 0) {
			state->curr_menu = MENU_MAIN;
			state->menu_needs_rerendering = true;
			return;
		}
		Camera *new_camera = state->cameras[sel-1];
		if (state->camera == new_camera) {
			// already using this camera- just change its precedence
			move_to_highest_precedence(&state->settings, state->camera);
		} else {
			camera_close(state->camera);
			state->camera = new_camera;
			PictureFormat picfmt = settings_picture_format_for_camera(state, state->camera);
			if (camera_open(state->camera, picfmt, settings_desired_framerate(state))) {
				// put at highest precedence
				move_to_highest_precedence(&state->settings, state->camera);
			} else {
				state->camera = NULL;
				select_camera(state);
			}
		}
	} else if (state->curr_menu == MENU_PIXFMT) {
		uint32_t *pixfmts = camera_get_pixfmts(state->camera);
		int sel = state->menu_sel[state->curr_menu];
		if (sel == 0) {
			state->curr_menu = MENU_MAIN;
			state->menu_needs_rerendering = true;
			return;
		}
		settings->pixel_format = pixfmts[sel-1];
		PictureFormat new_picfmt = settings_picture_format_for_camera(state, state->camera);
		arr_free(pixfmts);
		camera_set_format(state->camera,
			new_picfmt,
			settings_desired_framerate(state),
			0,
			false);
	} else if (state->curr_menu == MENU_HELP) {
		state->curr_menu = 0;
	} else if (state->curr_menu == MENU_SET_OUTPUT_DIR) {
		state->curr_menu = MENU_MAIN;
		state->menu_needs_rerendering = true;
	} else if (state->curr_menu == MENU_FRAMERATE) {
		int sel = state->menu_sel[MENU_FRAMERATE];
		int framerate = INT_MAX;
		if (sel > 0) {
			uint64_t supported = camera_framerates_supported(state->camera);
			for (framerate = 0; framerate < 64; framerate++) {
				if (supported & ((uint64_t)1 << framerate)) {
					if (--sel == 0) {
						break;
					}
				}
			}
		}
		if (framerate < 64) {
			PictureFormat picfmt = settings_picture_format_for_camera(state, state->camera);
			if (state->mode == MODE_VIDEO)
				settings->video_framerate = framerate;
			else
				settings->image_framerate = framerate;
			camera_set_format(state->camera, picfmt, framerate, 0, false);
		}
		state->curr_menu = MENU_MAIN;
		state->menu_needs_rerendering = true;
	}
}

static void select_camera(State *state) {
	Settings *settings = &state->settings;
	bool *cameras_working = calloc(1, arr_len(state->cameras));
	memset(cameras_working, 1, arr_len(state->cameras));
	while (true) {
		int camera_idx = -1;
		// find highest-precedence possibly-working camera
		arr_foreach_ptr(settings->camera_precedence, const Hash, h) {
			arr_foreach_ptr(state->cameras, Camera *const, pcamera) {
				Camera *c = *pcamera;
				if (hash_eq(camera_hash(c), *h)) {
					if (state->camera == c) {
						// already have best camera selected
						free(cameras_working);
						return;
					}
					camera_idx = (int)(pcamera - state->cameras);
					if (cameras_working[camera_idx]) {
						state->camera = c;
						break;
					}
				}
			}
			if (state->camera) break;
		}
		if (!state->camera) {
			// nothing in precedence list works- find first possibly-working camera
			for (camera_idx = 0; camera_idx < (int)arr_len(state->cameras); camera_idx++)
				if (cameras_working[camera_idx])
					break;
			if (camera_idx >= (int)arr_len(state->cameras)) {
				// no cameras work
				break;
			}
			state->camera = state->cameras[camera_idx];
		}
		if (camera_open(state->camera, settings_picture_format_for_camera(state, state->camera), settings_desired_framerate(state))) {
			bool already_there = false;
			arr_foreach_ptr(settings->camera_precedence, Hash, h) {
				if (hash_eq(*h, camera_hash(state->camera))) {
					already_there = true;
				}
			}
			// if hasn't already been added, put it at the lowest precedence
			if (!already_there) {
				arr_add(settings->camera_precedence, camera_hash(state->camera));
			}
			break;
		} else {
			cameras_working[camera_idx] = false;
			state->camera = NULL;
		}
	}
	free(cameras_working);
}

static int menu_get_option_at_pos(State *state, int x, int y) {
	// technically this may be wrong for a single frame when the menu options change, but who cares.
	arr_foreach_ptr(state->menu_option_rects, SDL_Rect, r) {
		if (SDL_PointInRect((const SDL_Point[1]){{x, y}}, r)) {
			int n = (int)(r - state->menu_option_rects);
			// important that we check this since rects may be out of date
			return n < menu_option_count(state) ? n : -1;
		}
	}
	return -1;
}

static bool mkdir_with_parents(const char *path) {
	if (mkdir(path, 0755) == 0
		|| errno == EEXIST)
		return true;
	char *buf = strdup(path);
	while (true) {
		size_t i;
		for (i = strlen(buf) - 1; i > 1; i--) {
			bool end = buf[i] == '/';
			buf[i] = '\0';
			if (end) break;
		}
		if (i == 1) {
			free(buf);
			return false;
		}
		if (mkdir(buf, 0755) == 0 || errno == EEXIST) {
			free(buf);
			return mkdir_with_parents(path);
		}
		if (errno != ENOENT) {
			perror("mkdir");
			free(buf);
			return false;
		}
	}
}

static void debug_print_device_attrs(struct udev_device *dev) {
	printf("----%s----\n",udev_device_get_devnode(dev));
	struct udev_list_entry *attr = NULL, *attrs = udev_device_get_sysattr_list_entry(dev);
	udev_list_entry_foreach(attr, attrs) {
		const char *val = udev_device_get_sysattr_value(dev, udev_list_entry_get_name(attr));
		printf("%s = %s\n", udev_list_entry_get_name(attr),
			val ? val : "NULL");
	}
}

static void get_cameras_from_udev_device(State *state, struct udev_device *dev) {
	const char *devnode = udev_device_get_devnode(dev);
	if (!devnode) return;
	const char *subsystem = udev_device_get_sysattr_value(dev, "subsystem");
	if (!subsystem || strcmp(subsystem, "video4linux") != 0) {
		// not a v4l device
		return;
	}
	int status = access(devnode, R_OK);
	if (status != 0 && errno == EACCES) {
		// can't read from this device
		return;
	}
	if (status != 0) {
		perror("access");
		return;
	}
	/*
	build up a serial number for the camera by taking its "serial" value,
	together with the serial of its ancestors
	(my personal camera doesn't have a serial on the video4linux device,
	 but does have one on its grandparent- this makes sense since a single
	 physical device can have multiple cameras, as well as microphones, etc.)
	*/
	// NOTE: we don't need to unref the return value of udev_device_get_parent
	struct udev_device *parent = udev_device_get_parent(dev);
	const char *serial_str = udev_device_get_sysattr_value(dev, "serial");
	StrBuilder serial = str_builder_new();
	if (serial_str && *serial_str)
		str_builder_appendf(&serial, "%s;", serial_str);
	for (int k = 0; k < 100 /* prevent infinite loop due to some fucked up device state */; k++) {
		const char *parent_serial = udev_device_get_sysattr_value(parent, "serial");
		if (parent_serial && strlen(parent_serial) >= 12 &&
			parent_serial[4] == ':' && parent_serial[7] == ':' && parent_serial[10] == '.') {
			// this is actually a USB interface! e.g. 0000:06:00.3
			// so it is not tied to the camera
			break;
		}
		if (parent_serial && *parent_serial)
			str_builder_appendf(&serial, "%s;", parent_serial);
		struct udev_device *grandparent = udev_device_get_parent(parent);
		if (!grandparent) break;
		parent = grandparent;
	}
	cameras_from_device(devnode, serial.str, &state->cameras);
	str_builder_free(&serial);
}

static char home_dir[PATH_MAX];
static bool get_expanded_output_dir(State *state, char path[PATH_MAX]) {
	Settings *settings = &state->settings;
	while (settings->output_dir && settings->output_dir[0] &&
		settings->output_dir[strlen(settings->output_dir) - 1] == '/') {
		settings->output_dir[strlen(settings->output_dir) - 1] = 0;
	}
	if (!settings->output_dir || !settings->output_dir[0]) {
		free(settings->output_dir);
		settings->output_dir = strdup(DEFAULT_OUTPUT_DIR);
	}
	if (settings->output_dir[0] == '~' && settings->output_dir[1] == '/') {
		snprintf(path, PATH_MAX, "%s/%s", home_dir, &settings->output_dir[2]);
	} else {
		snprintf(path, PATH_MAX, "%s", settings->output_dir);
	}
	return mkdir_with_parents(path);
}

static bool take_picture(State *state) {
	static char path[PATH_MAX];
	Settings *settings = &state->settings;
	// keep trying file names until we get one that doesn't exist yet
	for (size_t k = 0; ; k++) {
		if (!get_expanded_output_dir(state, path))
			return false;
		struct timespec ts = {0};
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm *tm = localtime((time_t[1]){ts.tv_sec});
		strftime(path + strlen(path), sizeof path - strlen(path), "/%Y-%m-%d-%H-%M-%S", tm);
		if (k > 0) {
			// add nanoseconds as well
			snprintf(path + strlen(path), sizeof path - strlen(path), "-%lu", (unsigned long)ts.tv_nsec);
		}
		const char *extension = state->mode == MODE_VIDEO ? "mp4" : image_format_extensions[settings->image_format];
		snprintf(path + strlen(path), sizeof path - strlen(path), ".%s", extension);
		int fd = open(path, O_EXCL | O_CREAT, 0644);
		if (fd == -1 && errno == EEXIST) {
			continue;
		} else if (fd == -1) {
			perror("can't write to picture directory");
			return false;
		} else {
			close(fd);
			break;
		}
	}
	bool success = false;
	switch (state->mode) {
	case MODE_IMAGE:
		switch (settings->image_format) {
		case IMG_FMT_JPEG:
			success = camera_save_jpg(state->camera, path, 90);
			break;
		case IMG_FMT_PNG:
			success = camera_save_png(state->camera, path);
			break;
		case IMG_FMT_COUNT:
			assert(false);
			break;
		}
		if (success) {
			state->flash_time = get_time_double();
		}
		break;
	case MODE_VIDEO:
		if (state->camera) {
			// TODO: adjustable video quality
			success = video_start(state->video, path,
				camera_frame_width(state->camera), camera_frame_height(state->camera),
				camera_framerate(state->camera), 5);
		}
		break;
	case MODE_COUNT:
		assert(false);
		break;
	}
	return success;
}

int main(void) {
	static State state_data;
	State *state = &state_data;
	Settings *settings = &state->settings;
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // if this program is sent a SIGTERM/SIGINT, don't turn it into a quit event
	if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
		fprintf(stderr, "couldn't initialize SDL\n");
		return EXIT_FAILURE;
	}
	if (sodium_init() < 0) {
		fatal_error("couldn't initialize libsodium");
	}
	if (!FcInit()) {
		fatal_error("couldn't initialize fontconfig");
	}
	#define FcFini "don't call FcFini: it's broken on certain versions of fontconfig - https://github.com/brndnmtthws/conky/pull/1755"
	if (TTF_Init() < 0) {
		fatal_error("couldn't initialize SDL2_ttf: %s\n", TTF_GetError());
	}
	{
		const char *home = getenv("HOME");
		if (home) {
			snprintf(home_dir, sizeof home_dir, "%s", home);
		} else {
			struct passwd *pwd = getpwuid(getuid());
			if (pwd) {
				snprintf(home_dir, sizeof home_dir, "%s", pwd->pw_dir);
			} else {
				perror("getpwuid");
				strcpy(home_dir, "."); // why not
			}
		}
	}
	char *font_path = NULL;
	{
		// find a suitable font
		FcPattern *pattern = FcPatternCreate();
		FcLangSet *langs = FcLangSetCreate();
		FcLangSetAdd(langs, (const FcChar8 *)"en-US");
		FcPatternAddLangSet(pattern, FC_LANG, langs);
		FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR);
		FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
		FcPatternAddInteger(pattern, FC_WIDTH, FC_WIDTH_NORMAL);
		FcPatternAddString(pattern, FC_FONTFORMAT, (const FcChar8 *)"TrueType");
		FcConfigSubstitute(0, pattern, FcMatchPattern); 
		FcDefaultSubstitute(pattern);
		FcResult result = 0;
		FcPattern *font = FcFontMatch(NULL, pattern, &result);
		if (result == FcResultMatch) {
			FcChar8 *file;
			if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch) {
				font_path = strdup((const char *)file);
			}
		} else {
			fatal_error("couldn't find any regular English TTF fonts. try installing one?");
		}
		FcPatternDestroy(pattern);
		FcPatternDestroy(font);
		FcLangSetDestroy(langs);
	}
	TTF_Font *font = TTF_OpenFont(font_path, 72);
	if (!font) {
		fatal_error("couldn't open font %s: %s", font_path, TTF_GetError());
	}
	SDL_Window *window = SDL_CreateWindow("camlet", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
	if (!window) {
		fatal_error("couldn't create window: %s", SDL_GetError());
	}
	state->video = video_init();

	static const struct {
		int maj, min;
	} gl_versions_to_try[] = {
		{4, 3},
		{3, 0}
	};
	
	SDL_GLContext glctx = NULL;
	for (size_t i = 0; !glctx && i < SDL_arraysize(gl_versions_to_try); i++) {
		gl.version_major = gl_versions_to_try[i].maj;
		gl.version_minor = gl_versions_to_try[i].min;
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl.version_major);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl.version_minor);
		#if DEBUG
		if (gl.version_major * 100 + gl.version_minor >= 403)
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
		#endif
		glctx = SDL_GL_CreateContext(window);
	}
	if (!glctx) {
		fatal_error("couldn't create GL context: %s", SDL_GetError());
	}
	SDL_GL_SetSwapInterval(1); // vsync
	#if __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
	#endif
	#define gl_get_proc(upper, lower) gl.lower = (PFNGL##upper##PROC)SDL_GL_GetProcAddress("gl" #lower);
	gl_for_each_proc(gl_get_proc);
	#if __GNUC__
	#pragma GCC diagnostic pop
	#endif
	camera_init(&gl);
	#if DEBUG
	{
		GLint flags = 0;
		gl.GetIntegerv(GL_CONTEXT_FLAGS, &flags);
		gl.Enable(GL_DEBUG_OUTPUT);
		gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
			// set up debug message callback
			gl.DebugMessageCallback(gl_message_callback, NULL);
			gl.DebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
		}
	}
	#endif
	gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	double last_time = get_time_double();
	GLuint textures[9] = {0};
	gl.GenTextures(SDL_arraysize(textures), textures);
	for (size_t i = 0; i < SDL_arraysize(textures); i++) {
		gl.BindTexture(GL_TEXTURE_2D, textures[i]);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	const GLuint menu_texture = textures[0];
	const GLuint no_camera_texture = textures[1];
	const GLuint debug_info_texture = textures[2];
	const GLuint black_texture = textures[6];
	const GLuint timer_texture = textures[7];
	const GLuint mode_texture = textures[8];
	{
		static uint8_t black[16];
		gl.BindTexture(GL_TEXTURE_2D, black_texture);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RED, 4, 4, 0, GL_RED, GL_UNSIGNED_BYTE, black);
	}
	// texture for camera output
	GLuint camera_textures[3] = {textures[3], textures[4], textures[5]};
	static const int32_t no_camera_width = 1280, no_camera_height = 720;
	{
		// create no camera texture
		int32_t w = no_camera_width, h = no_camera_height;
		SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 8, SDL_PIXELFORMAT_RGB24);
		SDL_LockSurface(surf);
		for (int32_t y = 0; y < h; y++) {
			uint8_t *row = &((uint8_t *)surf->pixels)[y * surf->pitch];
			uint8_t color = (uint8_t)(y * 255 / h);
			for (int32_t x = 0; x < w; x++, row += 3)
				*row = color;
		}
		SDL_UnlockSurface(surf);
		render_text_to_surface_anchored(font, surf, w / 2, h / 2, (SDL_Color){255, 255, 255, 255},
			"No Camera", 0, 0);
		SDL_LockSurface(surf);
		gl.BindTexture(GL_TEXTURE_2D, no_camera_texture);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, surf->pixels);
		SDL_UnlockSurface(surf);
		gl.GenerateMipmap(GL_TEXTURE_2D);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}
	const char *vshader_code = "attribute vec2 v_pos;\n\
attribute vec2 v_tex_coord;\n\
uniform vec2 u_scale;\n\
uniform vec2 u_offset;\n\
out vec2 tex_coord;\n\
void main() {\n\
	tex_coord = vec2(v_tex_coord.x, 1.0 - v_tex_coord.y);\n\
	gl_Position = vec4(u_scale * v_pos + u_offset, 0.0, 1.0);\n\
}\n\
";
	const char *fshader_code = "in vec4 color;\n\
in vec2 tex_coord;\n\
out vec4 o_color;\n\
uniform sampler2D u_sampler;\n\
uniform sampler2D u_sampler2;\n\
uniform sampler2D u_sampler3;\n\
uniform int u_pixel_format;\n\
uniform float u_flash;\n\
uniform float u_opacity;\n\
// SEE ALSO: identically-named function in camera.c\n\
vec3 ycbcr_ITU_R_601_to_rgb(vec3 ycbcr) {\n\
	mat4x3 cool_matrix = mat4x3(1.0,1.164,1.164,0.0,-0.378,2.107,1.596,-0.813,0.0,-0.864,0.525,-1.086);\n\
	// made up number tuned to my camera. probably can be inferred from v4l2_pix_format::xfer_func but that sounds annoying.\n\
	vec3 gamma = vec3(0.9,1.1,1.3);  \n\
	return clamp(pow(cool_matrix * vec4(ycbcr,1.0), gamma), 0.0, 1.0);\n\
}\n\
void main() {\n\
	vec3 color;\n\
	float opacity = u_opacity;\n\
	switch (u_pixel_format) {\n\
	case 0x59455247: // GREY\n\
		color = texture2D(u_sampler, tex_coord).xxx;\n\
		break;\n\
	case 0x47585858: // XXXGRAY (used for FPS display currently)\n\
		color = vec3(texture2D(u_sampler, tex_coord).w);\n\
		break;\n\
	case 0x56595559: { // YUYV 4:2:2 interleaved\n\
		ivec2 texsize = textureSize(u_sampler, 0);\n\
		vec2 tc = tex_coord * vec2(texsize);\n\
		ivec2 tc00 = ivec2(tc);\n\
		ivec2 tc10 = clamp(tc00 + ivec2(1, 0), ivec2(0), texsize - ivec2(1, 1));\n\
		ivec2 tc01 = clamp(tc00 + ivec2(0, 1), ivec2(0), texsize - ivec2(1, 1));\n\
		ivec2 tc11 = clamp(tc00 + ivec2(1, 1), ivec2(0), texsize - ivec2(1, 1));\n\
		vec2 tcfrac = tc - vec2(tc00);\n\
		vec4 t00 = texelFetch(u_sampler, tc00, 0);\n\
		vec4 t10 = texelFetch(u_sampler, tc10, 0);\n\
		vec4 t01 = texelFetch(u_sampler, tc01, 0);\n\
		vec4 t11 = texelFetch(u_sampler, tc11, 0);\n\
		vec2 cbcr0 = mix(t00.yw, t01.yw, tcfrac.y);\n\
		vec2 cbcr1 = mix(t10.yw, t11.yw, tcfrac.y);\n\
		vec2 cbcr = mix(cbcr0, cbcr1, tcfrac.x);\n\
		float y0, y1;\n\
		if (tcfrac.x < 0.5) {\n\
			y0 = mix(t00.x, t00.z, tcfrac.x * 2.0);\n\
			y1 = mix(t01.x, t01.z, tcfrac.x * 2.0);\n\
		} else {\n\
			y0 = mix(t00.z, t10.x, tcfrac.x * 2.0 - 1.0);\n\
			y1 = mix(t01.z, t11.x, tcfrac.x * 2.0 - 1.0);\n\
		}\n\
		float y = mix(y0, y1, tcfrac.y);\n\
		// technically we should check v4l2_pix_format::ycbcr_enc, but whatever.\n\
		color = ycbcr_ITU_R_601_to_rgb(vec3(y,cbcr));\n\
		} break;\n\
	case 0x32315559: // YUV 4:2:0 with separate planes\n\
	case 0x32315659: { // YVU 4:2:0 with separate planes (planes are reordered to YUV in camera.c)\n\
		float y = texture2D(u_sampler, tex_coord).x;\n\
		float cb = texture2D(u_sampler2, tex_coord).x;\n\
		float cr = texture2D(u_sampler3, tex_coord).x;\n\
		color = ycbcr_ITU_R_601_to_rgb(vec3(y,cb,cr));\n\
		} break;\n\
	case 0x3231564e: {// YUV 4:2:0 with a Y plane and a UV plane\n\
		float y = texture2D(u_sampler, tex_coord).x;\n\
		vec2 cbcr = texture2D(u_sampler2, tex_coord).xy;\n\
		color = ycbcr_ITU_R_601_to_rgb(vec3(y,cbcr));\n\
		} break;\n\
	case 0x3132564e: {// YVU 4:2:0 with a Y plane and a VU plane\n\
		float y = texture2D(u_sampler, tex_coord).x;\n\
		vec2 cbcr = texture2D(u_sampler2, tex_coord).yx;\n\
		color = ycbcr_ITU_R_601_to_rgb(vec3(y,cbcr));\n\
		} break;\n\
	case 0x34324241: { // RGBA32 (used for timer currently)\n\
		vec4 v = texture2D(u_sampler, tex_coord);\n\
		color = v.xyz;\n\
		opacity *= v.w;\n\
		} break;\n\
	default:\n\
		color = texture2D(u_sampler, tex_coord).xyz;\n\
		break;\n\
	}\n\
	o_color = vec4(mix(color, vec3(1.0), u_flash), opacity);\n\
}\n\
";
	static char shader_err[256] = {0};
	GLuint program = gl_compile_and_link_shaders(shader_err, vshader_code, fshader_code);
	if (*shader_err) {
		fatal_error("Couldn't compile shader: %s", shader_err);
	}
	if (program == 0) {
		fatal_error("Couldn't compile shader (no error log available)");
	}
	gl.BindFragDataLocation(program, 0, "o_color");
	GLuint vbo = 0, vao = 0;
	gl.GenBuffers(1, &vbo);
	gl.GenVertexArrays(1, &vao);
	const GLuint u_sampler = gl.GetUniformLocation(program, "u_sampler");
	const GLuint u_sampler2 = gl.GetUniformLocation(program, "u_sampler2");
	const GLuint u_sampler3 = gl.GetUniformLocation(program, "u_sampler3");
	const GLuint u_offset = gl.GetUniformLocation(program, "u_offset");
	const GLuint u_flash = gl.GetUniformLocation(program, "u_flash");
	const GLuint u_pixel_format = gl.GetUniformLocation(program, "u_pixel_format");
	const GLuint u_scale = gl.GetUniformLocation(program, "u_scale");
	const GLuint u_opacity = gl.GetUniformLocation(program, "u_opacity");
	const GLint v_pos = gl.GetAttribLocation(program, "v_pos");
	const GLint v_tex_coord = gl.GetAttribLocation(program, "v_tex_coord");
	{
		typedef struct {
			float pos[2];
			float tex_coord[2];
		} Vertex;
		typedef struct {
			Vertex v0;
			Vertex v1;
			Vertex v2;
		} Triangle;
		static const Triangle triangles[2] = {
			{ {{-1, -1}, {0, 0}}, {{1, 1}, {1, 1}}, {{-1, 1}, {0, 1}} },
			{ {{-1, -1}, {0, 0}}, {{1, -1}, {1, 0}}, {{1, 1}, {1, 1}} },
		};
		static const int ntriangles = sizeof triangles / sizeof triangles[0];
		gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
		gl.BindVertexArray(vao);
		gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ntriangles * sizeof(Triangle)), triangles, GL_STATIC_DRAW);
		gl.VertexAttribPointer(v_pos, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, pos));
		gl.EnableVertexAttribArray(v_pos);
		gl.VertexAttribPointer(v_tex_coord, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, tex_coord));
		gl.EnableVertexAttribArray(v_tex_coord);
	}
	struct udev *udev = udev_new();
	struct udev_monitor *udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	// subsystems don't seem to be set for "remove" events, so we shouldn't do this:
	//    udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "video4linux", NULL);
	if (!udev_monitor) {
		perror("udev_monitor_new_from_netlink");
	}
	if (udev_monitor) {
		// set udev monitor to nonblocking
		int fd = udev_monitor_get_fd(udev_monitor);
		int flags = fcntl(fd, F_GETFL);
		flags |= O_NONBLOCK | O_CLOEXEC;
		if (fcntl(fd, F_SETFL, flags) != 0) {
			perror("fcntl");
		}
		// enable monitor
		udev_monitor_enable_receiving(udev_monitor);
	}
	{
		struct udev_enumerate *enumerate = udev_enumerate_new(udev);
		udev_enumerate_add_match_subsystem(enumerate, "video4linux");
		udev_enumerate_scan_devices(enumerate);
		struct udev_list_entry *device = NULL, *devices = udev_enumerate_get_list_entry(enumerate);
		udev_list_entry_foreach(device, devices) {
			struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(device));
			if (!dev) continue;
			get_cameras_from_udev_device(state, dev);
			udev_device_unref(dev);
		}
		udev_enumerate_unref(enumerate);
		if (DEBUG) {
			printf("---CAMERAS---\n");
			for (size_t i = 0; i < arr_len(state->cameras); i++) {
				Camera *camera = state->cameras[i];
				printf("[%zu] %s ", i, camera_name(camera));
				char buf[HASH_SIZE * 2 + 1] = {0};
				camera_hash_str(camera, buf);
				printf("%s", buf);
				printf("\n");
			}
		}
	}
	if (!settings->output_dir)
		settings->output_dir = strdup(DEFAULT_OUTPUT_DIR);
	state->camera = NULL;
	state->flash_time = -INFINITY;
	if (arr_len(state->cameras) != 0) {
		select_camera(state);
	}
	uint32_t last_frame_pixfmt = 0;
	const int menu_options_per_column = 10;
	while (!state->quit) {
		state->menu_needs_rerendering = false;
		struct udev_device *dev = NULL;
		bool any_new_cameras = false;
		while (udev_monitor && (dev = udev_monitor_receive_device(udev_monitor))) {
			const char *devnode = udev_device_get_devnode(dev);
			const char *action = udev_device_get_action(dev);
			const char *subsystem = udev_device_get_sysattr_value(dev, "subsystem");
			if (strcmp(action, "remove") == 0) {
				if (state->camera && strcmp(devnode, camera_devnode(state->camera)) == 0) {
					// our special camera got disconnected ):
					video_stop(state->video);
					state->camera = NULL;
					if (state->curr_menu != MENU_HELP)
						state->curr_menu = 0;
				}
				for (size_t i = 0; i < arr_len(state->cameras); ) {
					if (strcmp(camera_devnode(state->cameras[i]), devnode) == 0) {
						arr_remove(state->cameras, i);
					} else {
						i++;
					}
				}
			} else if (strcmp(action, "add") == 0 && subsystem && strcmp(subsystem, "video4linux") == 0) {
				get_cameras_from_udev_device(state, dev);
				any_new_cameras = true;
			}
			udev_device_unref(dev);
		}
		if (!state->camera || (any_new_cameras && !video_is_recording(state->video)))
			select_camera(state);
		SDL_Event event = {0};
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) goto quit;
			if (event.type == SDL_KEYDOWN) switch (event.key.keysym.sym) {
			static char path[PATH_MAX];
			case SDLK_v:
				if (state->curr_menu == MENU_SET_OUTPUT_DIR && (event.key.keysym.mod & KMOD_CTRL) && settings->output_dir) {
					char *text = SDL_GetClipboardText();
					if (!text) break;
					if (strlen(text) + strlen(settings->output_dir) + 40 > PATH_MAX) {
						// too long
						break;
					}
					settings->output_dir =
						realloc(settings->output_dir, strlen(settings->output_dir) + 2 + strlen(text));
					strcat(settings->output_dir, text);
					state->menu_needs_rerendering = true;
					SDL_free(text);
				}
				break;
			case SDLK_f:
				if (event.key.keysym.mod & KMOD_CTRL) {
					if (!get_expanded_output_dir(state, path))
						break;
					if (fork() == 0) {
						execlp("xdg-open", "xdg-open", path, NULL);
						abort();
					}
				}
				break;
			case SDLK_TAB:
				if (state->curr_menu) break;
				if (video_is_recording(state->video)) break;
				state->mode = (state->mode + 1) % MODE_COUNT;
				switch (state->mode) {
				case MODE_IMAGE:
					camera_set_format(state->camera,
						settings_picture_format_for_camera(state, state->camera),
						settings_desired_framerate(state),
						0, false);
					break;
				case MODE_VIDEO: {
					PictureFormat picfmt = settings_picture_format_for_camera(state, state->camera);
					// force V4L2 to do the conversion if we have to
					picfmt.pixfmt = V4L2_PIX_FMT_YUV420;
					camera_set_format(state->camera, picfmt, settings_desired_framerate(state), 0, false);
					} break;
				case MODE_COUNT:
					assert(false);
					break;
				}
				break;
			case SDLK_SPACE:
				if (!state->camera || state->curr_menu != 0) break;
				if (video_is_recording(state->video)) {
					video_stop(state->video);
				} else {
					if (settings->timer == 0) {
						take_picture(state);
					} else {
						state->timer_activate_time = get_time_double();
					}
				}
				break;
			case SDLK_BACKSPACE:
				if (state->curr_menu == MENU_SET_OUTPUT_DIR && settings->output_dir) {
					if (event.key.keysym.mod & KMOD_CTRL) {
						settings->output_dir[0] = 0;
					} else if (settings->output_dir[0]) {
						settings->output_dir[strlen(settings->output_dir) - 1] = 0;
					}
					state->menu_needs_rerendering = true;
				}
				break;
			case SDLK_ESCAPE:
				if (state->curr_menu == MENU_MAIN || state->curr_menu == MENU_HELP) {
					state->curr_menu = MENU_NONE;
				} else if (video_is_recording(state->video)) {
					// don't allow opening menu while recording video
				} else {
					state->curr_menu = MENU_MAIN;
				}
				state->menu_needs_rerendering = true;
				break;
			case SDLK_UP: if (menu_option_count(state)) {
				state->menu_sel[state->curr_menu]--;
				if (state->menu_sel[state->curr_menu] < 0)
					state->menu_sel[state->curr_menu] += menu_option_count(state);
				state->menu_needs_rerendering = true;
				}
				break;
			case SDLK_DOWN: if (menu_option_count(state)) {
				state->menu_sel[state->curr_menu]++;
				if (state->menu_sel[state->curr_menu] >= menu_option_count(state))
					state->menu_sel[state->curr_menu] = 0;
				state->menu_needs_rerendering = true;
				}
				break;
			case SDLK_F1:
				state->curr_menu = state->curr_menu == MENU_HELP ? 0 : MENU_HELP;
				state->menu_needs_rerendering = true;
				break;
			case SDLK_F2:
				state->show_debug = !state->show_debug;
				break;
			case SDLK_LEFT:
				if (state->curr_menu == MENU_MAIN && main_menu[state->menu_sel[MENU_MAIN]] == MENU_OPT_IMGFMT) {
					settings->image_format = settings->image_format == 0 ? IMG_FMT_COUNT - 1 : settings->image_format - 1;
					state->menu_needs_rerendering = true;
				} else if (state->curr_menu == MENU_MAIN && main_menu[state->menu_sel[MENU_MAIN]] == MENU_OPT_TIMER) {
					change_timer(state, -1);
				} else if (menu_option_count(state) > menu_options_per_column) {
					int sel = state->menu_sel[state->curr_menu] - menu_options_per_column;
					if (sel < 0) {
						sel += (menu_option_count(state) + menu_options_per_column - 1)
							/ menu_options_per_column * menu_options_per_column;
					}
					while (sel >= menu_option_count(state))
						sel -= menu_options_per_column;
					state->menu_sel[state->curr_menu] = sel;
					state->menu_needs_rerendering = true;
				}
				break;
			case SDLK_RIGHT:
				if (menu_option_count(state) > menu_options_per_column) {
					int sel = state->menu_sel[state->curr_menu] + menu_options_per_column;
					if (sel >= menu_option_count(state))
						sel %= menu_options_per_column;
					state->menu_sel[state->curr_menu] = sel;
					state->menu_needs_rerendering = true;
					break;
				}
				if (state->curr_menu == MENU_MAIN)
					menu_select(state);
				if (state->curr_menu == MENU_MAIN && main_menu[state->menu_sel[MENU_MAIN]] == MENU_OPT_TIMER) {
					change_timer(state, 1);
				}
				break;
			case SDLK_RETURN:
				menu_select(state);
				break;
			}
			if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
				int mouse_x = event.button.x, mouse_y = event.button.y;
				if (state->curr_menu) {
					int opt = menu_get_option_at_pos(state, mouse_x, mouse_y);
					if (opt >= 0) {
						state->menu_sel[state->curr_menu] = opt;
						menu_select(state);
					}
				}
			}
			if (event.type == SDL_TEXTINPUT && state->curr_menu == MENU_SET_OUTPUT_DIR) {
				if (strlen(event.text.text) + strlen(settings->output_dir) + 40 > PATH_MAX) {
					// too long
				} else {
					settings->output_dir = realloc(settings->output_dir, strlen(settings->output_dir) + 2 + strlen(event.text.text));
					strcat(settings->output_dir, event.text.text);
					state->menu_needs_rerendering = true;
				}
			}
			if (state->quit) goto quit;
		}
		static int prev_window_width, prev_window_height;
		int window_width = 0, window_height = 0;
		// NOTE: not all window size changes seem to generate WINDOWEVENT_RESIZED.
		SDL_GetWindowSize(window, &window_width, &window_height);
		const bool window_size_changed = window_width != prev_window_width || window_height != prev_window_height;
		state->menu_needs_rerendering |= window_size_changed;
		prev_window_width = window_width;
		prev_window_height = window_height;
		int menu_width = window_width / 2, menu_height = window_height / 2;
		if (window_height * 16 > window_width * 9) {
			menu_width = menu_height * 16 / 9;
		}
		if (menu_width > window_width - 10) {
			menu_width = window_width - 10;
			menu_height = menu_width * 9 / 16;
		}
		if (menu_width < 70 || menu_height < 40) {
			// prevent division by zero, etc.
			// (but the menu will not be legible)
			menu_width = 64;
			menu_height = 36;
		}
		menu_width = (menu_width + 7) / 8 * 8; // play nice with pixel store alignment
		int font_size = menu_height / 20;
		TTF_SetFontSize(font, font_size);
		static int prev_hover_option = -1;
		int hover_option = -1;
		if (state->curr_menu) {
			// check if user is hovering over an option
			int mouse_x = 0, mouse_y = 0;
			SDL_GetMouseState(&mouse_x, &mouse_y);
			hover_option = menu_get_option_at_pos(state, mouse_x, mouse_y);
		} else {
			prev_hover_option = -1;
			arr_clear(state->menu_option_rects);
		}
		state->menu_needs_rerendering |= hover_option != prev_hover_option;
		prev_hover_option = hover_option;
		bool show_cursor = fmod(get_time_double(), 1.5) < 1.0;
		static int prev_show_cursor = -1;
		state->menu_needs_rerendering |= state->curr_menu == MENU_SET_OUTPUT_DIR && show_cursor != prev_show_cursor;
		prev_show_cursor = show_cursor;
		state->menu_needs_rerendering &= state->curr_menu != 0;
		if (state->menu_needs_rerendering) {
			// render menu
			arr_clear(state->menu_option_rects);
			SDL_Surface *menu = SDL_CreateRGBSurfaceWithFormat(0, menu_width, menu_height, 8, SDL_PIXELFORMAT_RGB24);
			SDL_FillRect(menu, NULL, 0x332244);
			SDL_Color text_color = {255, 255, 255, 255};
			SDL_Color highlight_color = {255, 255, 0, 255};
			SDL_Color hover_color = {0, 255, 255, 255};
			size_t n_options = menu_option_count(state);
			uint32_t *pixfmts = state->camera ? camera_get_pixfmts(state->camera) : NULL;
			PictureFormat *resolutions = state->camera ? camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera)) : NULL;
			const uint64_t framerates = state->camera ? camera_framerates_supported(state->camera) : 0;
			for (int opt_idx = 0; opt_idx < (int)n_options; opt_idx++) {
				char *option = NULL;
				switch (state->curr_menu) {
				case MENU_MAIN:
					switch (main_menu[opt_idx]) {
					case MENU_OPT_QUIT:
						option = strdup("Quit");
						break;
					case MENU_OPT_RESOLUTION:
						if (state->camera) {
							option = a_sprintf("Resolution: %" PRId32 "x%" PRId32,
								camera_frame_width(state->camera), camera_frame_height(state->camera));
						} else {
							option = a_sprintf("Resolution: None");
						}
						break;
					case MENU_OPT_VIDEO_INPUT:
						option = a_sprintf("Video Input: %s", state->camera ? camera_name(state->camera) : "None");
						break;
					case MENU_OPT_PIXFMT:
						option = a_sprintf("Picture format: %s",
							state->camera ? pixfmt_to_string(camera_pixel_format(state->camera))
								: "None");
						break;
					case MENU_OPT_IMGFMT:
						option = a_sprintf("Image format: %s", image_format_names[settings->image_format]);
						break;
					case MENU_OPT_SET_OUTPUT_DIR:
						option = a_sprintf("Output directory: %s", settings->output_dir);
						break;
					case MENU_OPT_FRAMERATE:
						option = state->camera ? a_sprintf("Frame rate: %d", camera_framerate(state->camera))
							: strdup("Frame rate: None");
						break;
					case MENU_OPT_TIMER:
						option = a_sprintf("Timer: %ds", settings->timer);
						break;
					default:
						assert(false);
						option = strdup("???");
					}
					break;
				case MENU_RESOLUTION:
					if (opt_idx == 0) {
						option = strdup("Back");
					} else {
						option = a_sprintf("%" PRId32 "x%" PRId32, resolutions[opt_idx-1].width, resolutions[opt_idx-1].height);
					}
					break;
				case MENU_INPUT:
					if (opt_idx == 0) {
						option = strdup("Back");
					} else {
						option = strdup(camera_name(state->cameras[opt_idx-1]));
					}
					break;
				case MENU_PIXFMT:
					if (opt_idx == 0) {
						option = strdup("Back");
					} else {
						option = a_sprintf("%s", pixfmt_to_string(pixfmts[opt_idx-1]));
					}
					break;
				case MENU_FRAMERATE: {
					if (opt_idx == 0) {
						option = strdup("Back");
					} else {
						int o = opt_idx;
						for (int i = 0; i < 64; i++) {
							if (framerates & ((uint64_t)1<< i)) {
								if (--o == 0) {
									option = a_sprintf("%d", i);
									break;
								}
							}
						}
						if (!option) option = strdup("???");
					}
					} break;
				case MENU_HELP:
				case MENU_SET_OUTPUT_DIR:
					option = a_sprintf("Back");
					break;
				case MENU_NONE:
				case MENU_COUNT:
					assert(false);
					break;
				}
				int n_columns = (n_options + menu_options_per_column - 1) / menu_options_per_column;
				int column_spacing = (menu_width - 10) / n_columns;
				SDL_Rect rect = render_text_to_surface(font, menu, 5 + (opt_idx / menu_options_per_column) * column_spacing,
					5 + (opt_idx % menu_options_per_column) * (5 + font_size),
					hover_option == opt_idx
					? hover_color
					: state->menu_sel[state->curr_menu] == opt_idx
					? highlight_color
					: text_color, option);
				// add menu position on screen
				rect.x += (window_width - menu_width) / 2;
				rect.y += (window_height - menu_height) / 2;
				arr_add(state->menu_option_rects, rect);
				free(option);
			}
			if (state->curr_menu == MENU_HELP) {
				const char *text[] = {
					"F1 - open this help screen",
					"F2 - show debug info",
					state->mode == MODE_VIDEO
						? "Space - start/stop recording"
						: "Space - take a picture",
					"Escape - open/close settings",
					"Ctrl+f - open picture directory",
					"Tab - switch between picture and video",
				};
				for (size_t line = 0; line < SDL_arraysize(text); line++) {
					render_text_to_surface(font, menu, 5, 5 + (5 + font_size) * (line + 1),
						text_color, text[line]);
				}
			} else if (state->curr_menu == MENU_SET_OUTPUT_DIR) {
				if (!settings->output_dir)
					settings->output_dir = strdup(DEFAULT_OUTPUT_DIR);
				SDL_Rect r = render_text_to_surface(font, menu, 5, 10 + font_size, text_color, settings->output_dir);
				if (show_cursor) {
					SDL_Rect cursor = {
						.x = r.x + r.w + 2,
						.y = r.y,
						.w = 1,
						.h = r.h
					};
					SDL_FillRect(menu, &cursor, sdl_color_to_u32(text_color));
				}
			}
			arr_free(pixfmts);
			arr_free(resolutions);
			gl.BindTexture(GL_TEXTURE_2D, menu_texture);
			SDL_LockSurface(menu);
			gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, menu_width, menu_height, 0, GL_RGB, GL_UNSIGNED_BYTE, menu->pixels);
			SDL_UnlockSurface(menu);
			SDL_FreeSurface(menu);
		}
		gl.Viewport(0, 0, window_width, window_height);
		gl.ClearColor(0, 0, 0, 1);
		gl.Clear(GL_COLOR_BUFFER_BIT);
		double curr_time = get_time_double();
		double frame_time = curr_time - last_time;
		last_time = curr_time;
		
		gl.UseProgram(program);
		gl.Uniform1i(u_sampler, 0);
		gl.Uniform1i(u_sampler2, 1);
		gl.Uniform1i(u_sampler3, 2);
		gl.Uniform1f(u_opacity, 1);
		gl.Uniform2f(u_offset, 0, 0);
		{
			// letterboxing
			const uint32_t frame_width = state->camera ? camera_frame_width(state->camera) : no_camera_width;
			const uint32_t frame_height = state->camera ? camera_frame_height(state->camera) : no_camera_height;
			if ((uint64_t)window_width * frame_height > (uint64_t)frame_width * window_height) {
				// window is wider than picture
				float letterbox_size = window_width - (float)window_height / frame_height * frame_width;
				letterbox_size /= window_width;
				gl.Uniform2f(u_scale, 1-letterbox_size, 1);
			} else if ((uint64_t)window_width * frame_height < (uint64_t)frame_width * window_height) {
				// window is narrower than picture
				float letterbox_size = window_height - (float)window_width / frame_width * frame_height;
				letterbox_size /= window_height;
				gl.Uniform2f(u_scale, 1, 1-letterbox_size);
			} else {
				// don't mess with fp inaccuracy
				gl.Uniform2f(u_scale, 1, 1);
			}
		}
		static double last_camera_time;
		if (last_camera_time == 0) last_camera_time = curr_time;
		static double smoothed_camera_time;
		if (state->camera) {
			static int n_active_textures;
			if (camera_next_frame(state->camera)) {
				last_frame_pixfmt = camera_pixel_format(state->camera);
				if (smoothed_camera_time == 0)
					smoothed_camera_time = curr_time - last_camera_time;
				// bias towards recent frame times
				smoothed_camera_time = smoothed_camera_time * 0.9 + (curr_time - last_camera_time) * 0.1;
				last_camera_time = curr_time;
				n_active_textures = camera_update_gl_textures(state->camera, camera_textures);
				if (video_is_recording(state->video)) {
					video_submit_frame(state->video, state->camera);
				}
			}
			gl.Uniform1i(u_pixel_format, last_frame_pixfmt);
			gl.ActiveTexture(GL_TEXTURE0);
			// we always want to bind something to every texture slot,
			// otherwise opengl will scream at us in the debug messages.
			if (n_active_textures >= 1) {
				gl.BindTexture(GL_TEXTURE_2D, camera_textures[0]);
			} else {
				gl.BindTexture(GL_TEXTURE_2D, black_texture);
			}
			gl.ActiveTexture(GL_TEXTURE1);
			if (n_active_textures >= 2) {
				gl.BindTexture(GL_TEXTURE_2D, camera_textures[1]);
			} else {
				gl.BindTexture(GL_TEXTURE_2D, black_texture);
			}
			gl.ActiveTexture(GL_TEXTURE2);
			if (n_active_textures >= 3) {
				gl.BindTexture(GL_TEXTURE_2D, camera_textures[2]);
			} else {
				gl.BindTexture(GL_TEXTURE_2D, black_texture);
			}
		} else {
			gl.ActiveTexture(GL_TEXTURE0);
			gl.BindTexture(GL_TEXTURE_2D, no_camera_texture);
			gl.Uniform1i(u_pixel_format, V4L2_PIX_FMT_RGB24);
		}
		double timer_time_left = settings->timer - (curr_time - state->timer_activate_time);
		if (state->timer_activate_time != 0 && timer_time_left <= 0) {
			take_picture(state);
			state->timer_activate_time = 0;
		}
		gl.Disable(GL_BLEND);
		gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
		gl.BindVertexArray(vao);
		gl.Uniform1f(u_flash, expf(-(curr_time - state->flash_time) * 3));
		gl.DrawArrays(GL_TRIANGLES, 0, 6);
		gl.Uniform1f(u_flash, 0);
		if (state->timer_activate_time != 0) {
			int time_displayed = (int)ceil(timer_time_left);
			static int prev_time_displayed = -1;
			static float gl_width, gl_height;
			gl.Enable(GL_BLEND);
			gl.ActiveTexture(GL_TEXTURE0);
			gl.BindTexture(GL_TEXTURE_2D, timer_texture);
			if (time_displayed != prev_time_displayed || window_size_changed) {
				static char text[16];
				TTF_SetFontSize(font, window_height / 4);
				snprintf(text, sizeof text, "%d", time_displayed);
				SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, (SDL_Color){255,255,255,255});
				SDL_LockSurface(surf);
				gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
				assert(surf->format->format == SDL_PIXELFORMAT_ARGB8888);
				assert(surf->pitch % 4 == 0);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, surf->pitch / 4);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				SDL_UnlockSurface(surf);
				gl_width = (float)surf->w / window_width;
				gl_height = (float)surf->h / window_height;
				SDL_FreeSurface(surf);
			}
			gl.Uniform2f(u_scale, gl_width, gl_height);
			gl.Uniform1i(u_sampler, 0);
			gl.Uniform1f(u_opacity, 0.9f);
			gl.Uniform2f(u_offset, 0, 0);
			gl.Uniform1i(u_pixel_format, V4L2_PIX_FMT_RGBA32);
			gl.DrawArrays(GL_TRIANGLES, 0, 6);
		}
		static char mode_text[32];
		if (window_size_changed) *mode_text = 0;
		if (state->mode == MODE_VIDEO) {
			const char *new_text = video_is_recording(state->video) ? "REC" : "VIDEO";
			static float gl_width, gl_height;
			gl.Enable(GL_BLEND);
			gl.ActiveTexture(GL_TEXTURE0);
			gl.BindTexture(GL_TEXTURE_2D, mode_texture);
			if (strcmp(mode_text, new_text) != 0) {
				strcpy(mode_text, new_text);
				TTF_SetFontSize(font, window_height / 10);
				SDL_Surface *surf = TTF_RenderUTF8_Blended(font, mode_text, (SDL_Color){0, 0, 255, 255});
				SDL_LockSurface(surf);
				gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
				assert(surf->format->format == SDL_PIXELFORMAT_ARGB8888);
				assert(surf->pitch % 4 == 0);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, surf->pitch / 4);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				SDL_UnlockSurface(surf);
				gl_width = (float)surf->w / window_width;
				gl_height = (float)surf->h / window_height;
				SDL_FreeSurface(surf);
			}
			gl.Uniform2f(u_scale, gl_width, gl_height);
			gl.Uniform2f(u_offset, 0.99f - gl_width, 1 - gl_height);
			gl.Uniform1i(u_sampler, 0);
			gl.Uniform1f(u_opacity, 1);
			gl.Uniform1i(u_pixel_format, V4L2_PIX_FMT_RGBA32);
			gl.DrawArrays(GL_TRIANGLES, 0, 6);
		}
		
		if (state->curr_menu) {
			gl.Enable(GL_BLEND);
			gl.ActiveTexture(GL_TEXTURE0);
			gl.BindTexture(GL_TEXTURE_2D, menu_texture);
			gl.Uniform2f(u_scale, (float)menu_width / window_width, (float)menu_height / window_height);
			gl.Uniform1i(u_sampler, 0);
			gl.Uniform1f(u_opacity, 0.9f);
			gl.Uniform2f(u_offset, 0, 0);
			gl.Uniform1i(u_pixel_format, V4L2_PIX_FMT_RGB24);
			gl.DrawArrays(GL_TRIANGLES, 0, 6);
		}
		if (state->show_debug) {
			static double smoothed_frame_time;
			if (smoothed_frame_time == 0)
				smoothed_frame_time = frame_time;
			// bias towards recent frame times
			smoothed_frame_time = smoothed_frame_time * 0.9 + frame_time * 0.1;
			static double last_fps_update = -INFINITY;
			gl.Enable(GL_BLEND);
			gl.ActiveTexture(GL_TEXTURE0);
			gl.BindTexture(GL_TEXTURE_2D, debug_info_texture);
			static float gl_width, gl_height;
			if (curr_time - last_fps_update > 0.5 || window_size_changed) {
				last_fps_update = curr_time;
				static char text[256];
				char hash[HASH_SIZE * 2 + 1] = {0};
				if (state->camera) {
					camera_hash_str(state->camera, hash);
					strcpy(&hash[8], "...");
				}
				snprintf(text, sizeof text, "Camera FPS: %" PRId32 "  Render FPS: %" PRId32 " Camera ID: %s",
					smoothed_camera_time > 1e-9 && smoothed_camera_time < 1 ? (int32_t)(1/smoothed_camera_time) : 0,
					smoothed_frame_time > 1e-9 && smoothed_frame_time < 1 ? (int32_t)(1/smoothed_frame_time) : 0,
					state->camera ? hash : "(None)");
				SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, (SDL_Color){255,255,255,255});
				SDL_LockSurface(surf);
				gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
				assert(surf->format->format == SDL_PIXELFORMAT_ARGB8888);
				assert(surf->pitch % 4 == 0);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, surf->pitch / 4);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				SDL_UnlockSurface(surf);
				gl_width = (float)surf->w / window_width;
				gl_height = (float)surf->h / window_height;
				SDL_FreeSurface(surf);
			}
			gl.Uniform2f(u_scale, gl_width, gl_height);
			gl.Uniform1i(u_sampler, 0);
			gl.Uniform1f(u_opacity, 0.9f);
			gl.Uniform2f(u_offset, 1 - gl_width, 1 - gl_height);
			gl.Uniform1i(u_pixel_format, PIX_FMT_XXXGRAY);
			gl.DrawArrays(GL_TRIANGLES, 0, 6);
		}
		SDL_GL_SwapWindow(window);
	}
	quit:
	video_quit(state->video);
	udev_monitor_unref(udev_monitor);
	udev_unref(udev);
	arr_foreach_ptr(state->cameras, Camera *, pcamera) {
		camera_free(*pcamera);
	}
	arr_free(state->cameras);
	free(settings->output_dir);
	arr_free(settings->camera_precedence);
	SDL_Quit();
	return 0;
}

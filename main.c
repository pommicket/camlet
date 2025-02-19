/*
TODO
-set saved image format
-add support for more pixfmts
-screen effect when picture is taken
-view previous pictures (thumbnails)
-click in menus
-left/right in resolution menu
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <time.h>
#include <libudev.h>
#include <sodium.h>
#include <fontconfig/fontconfig.h>
#include <fcntl.h>
#include <unistd.h>
#include "ds.h"
#include "camera.h"

// pixel format used for convenience
#define PIX_FMT_XXXGRAY 0x47585858

typedef enum {
	MENU_NONE,
	MENU_MAIN,
	MENU_RESOLUTION,
	MENU_INPUT,
	MENU_PIXFMT,
	MENU_HELP,
	MENU_COUNT
} Menu;

enum {
	MENU_OPT_QUIT = 1,
	MENU_OPT_RESOLUTION,
	MENU_OPT_INPUT,
	MENU_OPT_PIXFMT,
};
// use char for MenuOption type so that we can use strlen
typedef char MenuOption;
static const MenuOption main_menu[] = {
	MENU_OPT_INPUT,
	MENU_OPT_RESOLUTION,
	MENU_OPT_PIXFMT,
	MENU_OPT_QUIT,
	0
};

typedef struct {
	Menu curr_menu;
	int menu_sel[MENU_COUNT];
	bool show_fps;
	Camera *camera;
	Camera **cameras;
} State;

#if crypto_generichash_BYTES_MIN > HASH_SIZE
#error "crypto_generichash what happened"
#endif

static GlProcs gl;

#if DEBUG
static void APIENTRY gl_message_callback(GLenum source, GLenum type, unsigned int id, GLenum severity, 
	GLsizei length, const char *message, const void *userParam) {
	(void)source; (void)type; (void)id; (void)length; (void)userParam;
	if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
	printf("Message from OpenGL: %s.\n", message);
}
#endif

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
	if (program) {
		printf("Successfully linked program %u.\n", program);
	}
	return program;
}

static int menu_option_count(State *state) {
	switch (state->curr_menu) {
	case MENU_NONE: return 0;
	case MENU_HELP: return 1;
	case MENU_MAIN: return strlen(main_menu);
	case MENU_INPUT: return arr_len(state->cameras);
	case MENU_RESOLUTION: {
		PictureFormat *resolutions = camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera));
		int n = (int)arr_len(resolutions);
		arr_free(resolutions);
		return n;
		}
		break;
	case MENU_PIXFMT: {
		uint32_t *pixfmts = camera_get_pixfmts(state->camera);
		int n = (int)arr_len(pixfmts);
		arr_free(pixfmts);
		return n;
		}
		break;
	case MENU_COUNT: break;
	}
	assert(false);
	return 0;
}
static void render_text_to_surface_anchored(TTF_Font *font, SDL_Surface *dest, int x, int y, SDL_Color color, const char *str,
	int xanchor, int yanchor) {
	SDL_Surface *text = TTF_RenderUTF8_Blended(font, str, color);
	x -= (xanchor + 1) * text->w / 2;
	y -= (yanchor + 1) * text->h / 2;
	SDL_BlitSurface(text, NULL, dest, (SDL_Rect[1]){{x, y, 0, 0}});
	SDL_FreeSurface(text);
}

static void render_text_to_surface(TTF_Font *font, SDL_Surface *dest, int x, int y, SDL_Color color, const char *str) {
	render_text_to_surface_anchored(font, dest, x, y, color, str, -1, -1);
}

int main(void) {
	static State state_data;
	State *state = &state_data;
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // if this program is sent a SIGTERM/SIGINT, don't turn it into a quit event
	if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
		fprintf(stderr, "couldn't initialize SDL\n");
		return EXIT_FAILURE;
	}
	if (sodium_init() < 0) {
		fprintf(stderr, "couldn't initialize libsodium\n");
		return EXIT_FAILURE;
	}
	if (!FcInit()) {
		fprintf(stderr, "couldn't initialize fontconfig\n");
		return EXIT_FAILURE;
	}
	#define FcFini "don't call FcFini: it's broken on certain versions of fontconfig - https://github.com/brndnmtthws/conky/pull/1755"
	if (TTF_Init() < 0) {
		fprintf(stderr, "couldn't initialize SDL2_ttf: %s\n", TTF_GetError());
		return EXIT_FAILURE;
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
			fprintf(stderr, "couldn't find any regular English TTF fonts. try installing one?\n");
			return EXIT_FAILURE;
		}
		FcPatternDestroy(pattern);
		FcPatternDestroy(font);
		FcLangSetDestroy(langs);
	}
	TTF_Font *font = TTF_OpenFont(font_path, 72);
	if (!font) {
		fprintf(stderr, "couldn't open font %s: %s\n", font_path, TTF_GetError());
		return EXIT_FAILURE;
	}
	SDL_Window *window = SDL_CreateWindow("camlet", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
	if (!window) {
		fprintf(stderr, "couldn't create window: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	int gl_version_major = 3, gl_version_minor = 0;
	#if DEBUG
		gl_version_major = 4;
		gl_version_minor = 3;
	#endif
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl_version_major);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl_version_minor);
	SDL_GLContext glctx = SDL_GL_CreateContext(window);
	if (!glctx) {
		fprintf(stderr, "couldn't create GL context: %s\n", SDL_GetError());
		return EXIT_FAILURE;
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
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double last_time = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
	GLuint textures[4] = {0};
	gl.GenTextures(SDL_arraysize(textures), textures);
	for (size_t i = 0; i < SDL_arraysize(textures); i++) {
		gl.BindTexture(GL_TEXTURE_2D, textures[i]);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	// texture for camera output
	GLuint texture = textures[0];
	GLuint menu_texture = textures[1];
	GLuint no_camera_texture = textures[2];
	GLuint fps_texture = textures[3];
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
uniform sampler2D u_sampler;\n\
uniform int u_pixel_format;\n\
uniform float u_opacity;\n\
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
	default:\n\
		color = texture2D(u_sampler, tex_coord).xyz;\n\
		break;\n\
	}\n\
	gl_FragColor = vec4(color, opacity);\n\
}\n\
";
	char err[256] = {0};
	GLuint program = gl_compile_and_link_shaders(err, vshader_code, fshader_code);
	if (*err) {
		fprintf(stderr, "%s\n",err);
	}
	if (program == 0) return EXIT_FAILURE;
	GLuint vbo = 0, vao = 0;
	gl.GenBuffers(1, &vbo);
	gl.GenVertexArrays(1, &vao);
	const GLuint u_sampler = gl.GetUniformLocation(program, "u_sampler");
	const GLuint u_offset = gl.GetUniformLocation(program, "u_offset");
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
	udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "video4linux", NULL);
	if (!udev_monitor) {
		perror("udev_monitor_new_from_netlink");
	}
	if (udev_monitor) {
		// set udev monitor to nonblocking
		int fd = udev_monitor_get_fd(udev_monitor);
		int flags = fcntl(fd, F_GETFL);
		flags |= O_NONBLOCK;
		if (fcntl(fd, F_SETFL, flags) != 0) {
			perror("fcntl");
		}
		// enable monitor
		udev_monitor_enable_receiving(udev_monitor);
	}
	{
		struct udev_enumerate *enumerate = udev_enumerate_new(udev);
		udev_enumerate_add_match_subsystem(enumerate, "video4linux");
		/*
		udev_enumerate_add_match_subsystem(enumerate, "usb");
		udev_list_entry_foreach(device, devices) {
			const char *serial = udev_device_get_sysattr_value(dev, "serial");
			if (!serial || !*serial) continue;
			TODO: walk through device directory here to see if it has any video4linux children.
			NOTE: bus_info seems to be not a good way of identifying devices (it's a bit mysterious)
			      and we'd have to support nested USB hubs which is a pain anyways.
		}
		*/
		udev_enumerate_scan_devices(enumerate);
		struct udev_list_entry *device = NULL, *devices = udev_enumerate_get_list_entry(enumerate);
		udev_list_entry_foreach(device, devices) {
			struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(device));
			if (!dev) continue;
			const char *devnode = udev_device_get_devnode(dev);
			if (!devnode) continue;
			const char *subsystem = udev_device_get_sysattr_value(dev, "subsystem");
			const char *serial = udev_device_get_sysattr_value(dev, "serial");
			if (strcmp(subsystem, "video4linux") == 0) {
				int status = access(devnode, R_OK);
				if (status != 0 && errno == EACCES) {
					// can't read from this device
					goto cont;
				}
				if (status) break;
				printf("---%s\n",devnode);
				cameras_from_device(devnode, serial, &state->cameras);
			}
			cont:
			udev_device_unref(dev);
		}
		udev_enumerate_unref(enumerate);
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
	if (arr_len(state->cameras) == 0) {
		state->camera = NULL;
	} else {
		state->camera = state->cameras[0];
		if (!camera_open(state->camera))
			return EXIT_FAILURE;
	}
	while(true) {
		struct udev_device *dev = NULL;
		while (udev_monitor && (dev = udev_monitor_receive_device(udev_monitor))) {
			printf("%s %s\n",udev_device_get_sysname(dev),udev_device_get_action(dev));
			struct udev_list_entry *attr = NULL, *attrs = udev_device_get_sysattr_list_entry(dev);
			
			udev_list_entry_foreach(attr, attrs) {
				printf("%s = %s\n", udev_list_entry_get_name(attr),
					udev_device_get_sysattr_value(dev, udev_list_entry_get_name(attr)));
			}
			udev_device_unref(dev);
		}
		SDL_Event event={0};
		bool menu_needs_rerendering = false;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) goto quit;
			if (event.type == SDL_KEYDOWN) switch (event.key.keysym.sym) {
			case SDLK_SPACE: {
				time_t t = time(NULL);
				struct tm *tm = localtime(&t);
				char name[256];
				strftime(name, sizeof name, "%Y-%m-%d-%H-%M-%S.jpg", tm);
				camera_write_jpg(state->camera, name, 90);
				} break;
			case SDLK_ESCAPE:
				if (state->curr_menu == MENU_NONE) {
					state->curr_menu = MENU_MAIN;
				} else if (state->curr_menu == MENU_MAIN || state->curr_menu == MENU_HELP) {
					state->curr_menu = MENU_NONE;
				} else {
					state->curr_menu = MENU_MAIN;
				}
				menu_needs_rerendering = true;
				break;
			case SDLK_UP: if (menu_option_count(state)) {
				state->menu_sel[state->curr_menu]--;
				if (state->menu_sel[state->curr_menu] < 0)
					state->menu_sel[state->curr_menu] += menu_option_count(state);
				menu_needs_rerendering = true;
				}
				break;
			case SDLK_DOWN: if (menu_option_count(state)) {
				state->menu_sel[state->curr_menu]++;
				if (state->menu_sel[state->curr_menu] >= menu_option_count(state))
					state->menu_sel[state->curr_menu] = 0;
				menu_needs_rerendering = true;
				}
				break;
			case SDLK_F1:
				state->curr_menu = state->curr_menu == MENU_HELP ? 0 : MENU_HELP;
				menu_needs_rerendering = true;
				break;
			case SDLK_F2:
				state->show_fps = !state->show_fps;
				break;
			case SDLK_RIGHT:
			case SDLK_RETURN:
				if (state->curr_menu == MENU_MAIN) {
					switch (main_menu[state->menu_sel[MENU_MAIN]]) {
					case MENU_OPT_QUIT:
						goto quit;
					case MENU_OPT_RESOLUTION: if (state->camera) {
						state->curr_menu = MENU_RESOLUTION;
						menu_needs_rerendering = true;
						// set menu_sel
						PictureFormat *resolutions = camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera));
						arr_foreach_ptr(resolutions, PictureFormat, resolution) {
							if (resolution->width == camera_frame_width(state->camera)
								&& resolution->height == camera_frame_height(state->camera)) {
								state->menu_sel[MENU_RESOLUTION] = (int)(resolution - resolutions);
							}
						}
						arr_free(resolutions);
						} break;
					case MENU_OPT_INPUT:
						if (state->cameras) {
							state->curr_menu = MENU_INPUT;
							menu_needs_rerendering = true;
							state->menu_sel[MENU_INPUT] = 0;
							arr_foreach_ptr(state->cameras, Camera *, pcam) {
								if (*pcam == state->camera) {
									state->menu_sel[MENU_INPUT] = (int)(pcam - state->cameras);
								}
							}
						}
						break;
					case MENU_OPT_PIXFMT: if (state->camera) {
						state->curr_menu = MENU_PIXFMT;
						menu_needs_rerendering = true;
						// set menu_sel
						uint32_t *pixfmts = camera_get_pixfmts(state->camera);
						arr_foreach_ptr(pixfmts, uint32_t, pixfmt) {
							if (*pixfmt == camera_pixel_format(state->camera)) {
								state->menu_sel[MENU_PIXFMT] = (int)(pixfmt - pixfmts);
							}
						}
						arr_free(pixfmts);
						} break;
					}
				} else if (state->curr_menu == MENU_RESOLUTION) {
					PictureFormat *resolutions = camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera));
					camera_set_format(state->camera,
						resolutions[state->menu_sel[state->curr_menu]],
						camera_access_method(state->camera),
						false);
					arr_free(resolutions);
				} else if (state->curr_menu == MENU_INPUT) {
					Camera *new_camera = state->cameras[state->menu_sel[MENU_INPUT]];
					if (state->camera == new_camera) {
						// already using this camera
					} else {
						camera_close(state->camera);
						state->camera = new_camera;
						camera_open(state->camera);
					}
				} else if (state->curr_menu == MENU_PIXFMT) {
					uint32_t *pixfmts = camera_get_pixfmts(state->camera);
					uint32_t pixfmt = pixfmts[state->menu_sel[state->curr_menu]];
					PictureFormat new_picfmt = camera_closest_resolution(state->camera, pixfmt, camera_frame_width(state->camera), camera_frame_height(state->camera));
					arr_free(pixfmts);
					camera_set_format(state->camera,
						new_picfmt,
						camera_access_method(state->camera),
						false);
				} else if (state->curr_menu == MENU_HELP) {
					state->curr_menu = 0;
				}
				break;
			}
		}
		static int prev_window_width, prev_window_height;
		int window_width = 0, window_height = 0;
		SDL_GetWindowSize(window, &window_width, &window_height);
		// not all window size changes seem to generate WINDOWEVENT_RESIZED.
		menu_needs_rerendering |= window_width != prev_window_width;
		menu_needs_rerendering |= window_height != prev_window_height;
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
		menu_needs_rerendering &= state->curr_menu != 0;
		int font_size = menu_height / 20;
		TTF_SetFontSize(font, font_size);	
		if (menu_needs_rerendering) {
			// render menu
			SDL_Surface *menu = SDL_CreateRGBSurfaceWithFormat(0, menu_width, menu_height, 8, SDL_PIXELFORMAT_RGB24);
			SDL_FillRect(menu, NULL, 0x332244);
			SDL_Color text_color = {255, 255, 255, 255};
			SDL_Color highlight_color = {255, 255, 0, 255};
			size_t n_options = menu_option_count(state);
			uint32_t *pixfmts = state->camera ? camera_get_pixfmts(state->camera) : NULL;
			PictureFormat *resolutions = state->camera ? camera_get_resolutions_with_pixfmt(state->camera, camera_pixel_format(state->camera)) : NULL;
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
					case MENU_OPT_INPUT:
						option = a_sprintf("Input: %s", state->camera ? camera_name(state->camera) : "None");
						break;
					case MENU_OPT_PIXFMT:
						option = a_sprintf("Picture format: %s",
							state->camera ? pixfmt_to_string(camera_pixel_format(state->camera))
								: "None");
						break;
					default:
						assert(false);
						option = strdup("???");
					}
					break;
				case MENU_RESOLUTION:
					option = a_sprintf("%" PRId32 "x%" PRId32, resolutions[opt_idx].width, resolutions[opt_idx].height);
					break;
				case MENU_INPUT:
					option = strdup(camera_name(state->cameras[opt_idx]));
					break;
				case MENU_PIXFMT:
					option = a_sprintf("%s", pixfmt_to_string(pixfmts[opt_idx]));
					break;
				case MENU_HELP:
					option = a_sprintf("Back");
					break;
				case MENU_NONE:
				case MENU_COUNT:
					assert(false);
					break;
				}
				int options_per_column = 10;
				int n_columns = (n_options + options_per_column - 1) / options_per_column;
				int column_spacing = (menu_width - 10) / n_columns;
				render_text_to_surface(font, menu, 5 + (opt_idx / options_per_column) * column_spacing,
					5 + (opt_idx % options_per_column) * (5 + font_size),
					state->menu_sel[state->curr_menu] == opt_idx
					? highlight_color
					: text_color, option);
				free(option);
			}
			if (state->curr_menu == MENU_HELP) {
				const char *text[] = {
					"F1 - open this help screen",
					"Space - take a picture",
					"Escape - open/close settings",
					"F2 - show frame rate",
				};
				for (size_t line = 0; line < SDL_arraysize(text); line++) {
					render_text_to_surface(font, menu, 5, 5 + (5 + font_size) * (line + 1),
						text_color, text[line]);
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
		clock_gettime(CLOCK_MONOTONIC, &ts);
		double curr_time = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
		double frame_time = curr_time - last_time;
		last_time = curr_time;
		
		gl.UseProgram(program);
		gl.ActiveTexture(GL_TEXTURE0);
		gl.Uniform1i(u_sampler, 0);
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
		if (state->camera) {
			gl.BindTexture(GL_TEXTURE_2D, texture);
			if (camera_next_frame(state->camera)) {
				camera_update_gl_texture_2d(state->camera);
			}
			gl.Uniform1i(u_pixel_format, camera_pixel_format(state->camera));
		} else {
			gl.BindTexture(GL_TEXTURE_2D, no_camera_texture);
			gl.Uniform1i(u_pixel_format, V4L2_PIX_FMT_RGB24);
		}
		gl.Disable(GL_BLEND);
		gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
		gl.BindVertexArray(vao);
		gl.DrawArrays(GL_TRIANGLES, 0, 6);
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
		static double smoothed_frame_time;
		if (smoothed_frame_time == 0)
			smoothed_frame_time = frame_time;
		// bias towards recent frame times
		smoothed_frame_time = smoothed_frame_time * 0.9 + frame_time * 0.1;
		if (state->show_fps) {
			static double last_fps_update = -INFINITY;
			gl.Enable(GL_BLEND);
			gl.ActiveTexture(GL_TEXTURE0);
			gl.BindTexture(GL_TEXTURE_2D, fps_texture);
			static float gl_width, gl_height;
			if (curr_time - last_fps_update > 0.5) {
				last_fps_update = curr_time;
				static char text[32];
				snprintf(text, sizeof text, "FPS: %" PRId32,
					smoothed_frame_time > 1e-9 && smoothed_frame_time < 1 ? (int32_t)(1/smoothed_frame_time)
						: 0);
				SDL_Surface *fps = TTF_RenderUTF8_Blended(font, text, (SDL_Color){255,255,255,255});
				SDL_LockSurface(fps);
				gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
				assert(fps->pitch % 4 == 0);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, fps->pitch / 4);
				gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fps->w, fps->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fps->pixels);
				gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				SDL_UnlockSurface(fps);
				gl_width = (float)fps->w / window_width;
				gl_height = (float)fps->h / window_height;
				SDL_FreeSurface(fps);
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
	udev_monitor_unref(udev_monitor);
	udev_unref(udev);
	//debug_save_24bpp_bmp("out.bmp", buf, camera->best_format.fmt.pix.width, camera->best_format.fmt.pix.height);
	arr_foreach_ptr(state->cameras, Camera *, pcamera) {
		camera_free(*pcamera);
	}
	arr_free(state->cameras);
	SDL_Quit();
	return 0;
}

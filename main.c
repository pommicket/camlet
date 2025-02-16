#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <libv4l2.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <SDL.h>
#include <time.h>
#include <stdbool.h>
#include <libudev.h>
#include <sodium.h>
#include <GL/glcorearb.h>
#include "ds.h"
#include "lib/stb_image_write.h"

#define HASH_SIZE 16
#if crypto_generichash_BYTES_MIN > HASH_SIZE
#error "crypto_generichash what happened"
#endif
typedef struct {
	uint8_t hash[HASH_SIZE];
} Hash;

typedef struct {
	char *dev_path;
	char *name;
	uint32_t input_idx;
	struct v4l2_format best_format;
	crypto_generichash_state hash_state;
	int usb_busnum;
	int usb_devnum;
	int usb_devpath;
	Hash hash;
} Camera;

/// macro trickery to avoid having to write every GL function multiple times
#define gl_for_each_proc(do)\
	do(DRAWARRAYS, DrawArrays)\
	do(GENTEXTURES, GenTextures)\
	do(DELETETEXTURES, DeleteTextures)\
	do(GENERATEMIPMAP, GenerateMipmap)\
	do(TEXIMAGE2D, TexImage2D)\
	do(BINDTEXTURE, BindTexture)\
	do(TEXPARAMETERI, TexParameteri)\
	do(GETERROR, GetError)\
	do(GETINTEGERV, GetIntegerv)\
	do(ENABLE, Enable)\
	do(DISABLE, Disable)\
	do(BLENDFUNC, BlendFunc)\
	do(VIEWPORT, Viewport)\
	do(CLEARCOLOR, ClearColor)\
	do(CLEAR, Clear)\
	do(FINISH, Finish)\
	do(CREATESHADER, CreateShader)\
	do(DELETESHADER, DeleteShader)\
	do(CREATEPROGRAM, CreateProgram)\
	do(SHADERSOURCE, ShaderSource)\
	do(GETSHADERIV, GetShaderiv)\
	do(GETSHADERINFOLOG, GetShaderInfoLog)\
	do(COMPILESHADER, CompileShader)\
	do(CREATEPROGRAM, CreateProgram)\
	do(DELETEPROGRAM, DeleteProgram)\
	do(ATTACHSHADER, AttachShader)\
	do(LINKPROGRAM, LinkProgram)\
	do(GETPROGRAMIV, GetProgramiv)\
	do(GETPROGRAMINFOLOG, GetProgramInfoLog)\
	do(USEPROGRAM, UseProgram)\
	do(GETATTRIBLOCATION, GetAttribLocation)\
	do(GETUNIFORMLOCATION, GetUniformLocation)\
	do(GENBUFFERS, GenBuffers)\
	do(DELETEBUFFERS, DeleteBuffers)\
	do(BINDBUFFER, BindBuffer)\
	do(BUFFERDATA, BufferData)\
	do(VERTEXATTRIBPOINTER, VertexAttribPointer)\
	do(ENABLEVERTEXATTRIBARRAY, EnableVertexAttribArray)\
	do(DISABLEVERTEXATTRIBARRAY, DisableVertexAttribArray)\
	do(GENVERTEXARRAYS, GenVertexArrays)\
	do(DELETEVERTEXARRAYS, DeleteVertexArrays)\
	do(BINDVERTEXARRAY, BindVertexArray)\
	do(ACTIVETEXTURE, ActiveTexture)\
	do(UNIFORM1F, Uniform1f)\
	do(UNIFORM2F, Uniform2f)\
	do(UNIFORM3F, Uniform3f)\
	do(UNIFORM4F, Uniform4f)\
	do(UNIFORM1I, Uniform1i)\
	do(UNIFORM2I, Uniform2i)\
	do(UNIFORM3I, Uniform3i)\
	do(UNIFORM4I, Uniform4i)\
	do(UNIFORMMATRIX4FV, UniformMatrix4fv)\
	do(DEBUGMESSAGECALLBACK, DebugMessageCallback)\
	do(DEBUGMESSAGECONTROL, DebugMessageControl)\
	do(PIXELSTOREI, PixelStorei)
#define gl_define_proc(upper, lower) static PFNGL##upper##PROC gl_##lower;
gl_for_each_proc(gl_define_proc)
#undef gl_define_proc

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

// compile a GLSL shader
GLuint gl_compile_shader(char error_buf[256], const char *code, GLenum shader_type) {
	GLuint shader = gl_CreateShader(shader_type);
	char header[128];
	snprintf(header, sizeof header, "#version 130\n\
#line 1\n");
	const char *sources[2] = {
		header,
		code
	};
	gl_ShaderSource(shader, 2, sources, NULL);
	gl_CompileShader(shader);
	GLint status = 0;
	gl_GetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		char log[1024] = {0};
		gl_GetShaderInfoLog(shader, sizeof log - 1, NULL, log);
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
	GLuint program = gl_CreateProgram();
	if (program) {
		for (size_t i = 0; i < count; ++i) {
			if (!shaders[i]) {
				gl_DeleteProgram(program);
				return 0;
			}
			gl_AttachShader(program, shaders[i]);
		}
		gl_LinkProgram(program);
		GLint status = 0;
		gl_GetProgramiv(program, GL_LINK_STATUS, &status);
		if (status == GL_FALSE) {
			char log[1024] = {0};
			gl_GetProgramInfoLog(program, sizeof log - 1, NULL, log);
			if (error_buf) {
				snprintf(error_buf, 256, "Error linking shaders: %s", log);
			} else {
				printf("Error linking shaders: %s\n", log);
			}
			gl_DeleteProgram(program);
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
	if (shaders[0]) gl_DeleteShader(shaders[0]);
	if (shaders[1]) gl_DeleteShader(shaders[1]);
	if (program) {
		printf("Successfully linked program %u.\n", program);
	}
	return program;
}


void get_cameras_from_device(const char *dev_path, const char *serial, int fd, Camera **cameras) {
	struct v4l2_capability cap = {0};
	v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) return;
	struct v4l2_input input = {0};
	for (uint32_t input_idx = 0; ; input_idx++) {
		input.index = input_idx;
		if (v4l2_ioctl(fd, VIDIOC_ENUMINPUT, &input) == -1) break;
		if (input.type != V4L2_INPUT_TYPE_CAMERA) continue;
		Camera camera = {0};
		crypto_generichash_init(&camera.hash_state, NULL, 0, HASH_SIZE);
		crypto_generichash_update(&camera.hash_state, cap.card, strlen((const char *)cap.card) + 1);
		crypto_generichash_update(&camera.hash_state, input.name, strlen((const char *)input.name) + 1);
		struct v4l2_fmtdesc fmtdesc = {0};
		struct v4l2_format best_format = {0};
		for (uint32_t fmt_idx = 0; ; fmt_idx++) {
			fmtdesc.index = fmt_idx;
			fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == -1) break;
			uint32_t fourcc[2] = {fmtdesc.pixelformat, 0};
			printf("  - %s (%s)\n",fmtdesc.description, (const char *)fourcc);
			struct v4l2_frmsizeenum frmsize = {0};
			if (serial && *serial)
				crypto_generichash_update(&camera.hash_state, (const uint8_t *)serial, strlen(serial) + 1);
			const char *bus_info = (const char *)cap.bus_info;
			int usb_busnum = 0;
			int usb_devnum = 0;
			int usb_devpath = 0;
			if (strlen(bus_info) >= 18 && strlen(bus_info) <= 20 &&
				// what are those mystery 0s in the bus_info referring to.. . who knows...
				sscanf(bus_info, "usb-0000:%d:00.%d-%d", &usb_busnum, &usb_devnum, &usb_devpath) == 3) {
				camera.usb_busnum = usb_busnum;
				camera.usb_devnum = usb_devnum;
				camera.usb_devpath = usb_devpath;
			} else {
				camera.usb_busnum = -1;
				camera.usb_devnum = -1;
				camera.usb_devpath = -1;
			}
			for (uint32_t frmsz_idx = 0; ; frmsz_idx++) {
				frmsize.index = frmsz_idx;
				frmsize.pixel_format = fmtdesc.pixelformat;
				if (v4l2_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == -1) break;
				// Now you might think do we really need this?
				// Is it really not enough to use the device name, input name, and serial number to uniquely identify a camera??
				// No. you fool. Of course there is a Logitech camera with an infrared sensor (for face recognition)
				// that shows up as two video devices with identical names, capabilities, input names, etc. etc.
				// and the only way to distinguish them is the picture formats they support.
				// Oddly Windows doesn't show the infrared camera as an input device.
				// I wonder if there is some way of detecting which one is the "normal" camera.
				// Or perhaps Windows has its own special proprietary driver and we have no way of knowing.
				crypto_generichash_update(&camera.hash_state, (const uint8_t *)&frmsz_idx, sizeof frmsz_idx);
				crypto_generichash_update(&camera.hash_state, (const uint8_t *)&frmsize.pixel_format, sizeof frmsize.pixel_format);
				// are there even any stepwise cameras out there?? who knows.
				uint32_t frame_width = frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ? frmsize.discrete.width : frmsize.stepwise.max_width;
				uint32_t frame_height = frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE ? frmsize.discrete.height : frmsize.stepwise.max_height;
				crypto_generichash_update(&camera.hash_state, (const uint8_t *)&frame_width, sizeof frame_width);
				crypto_generichash_update(&camera.hash_state, (const uint8_t *)&frame_height, sizeof frame_height);
if (fmtdesc.pixelformat == V4L2_PIX_FMT_RGB24) {
	struct v4l2_format format = {0};
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.field = V4L2_FIELD_ANY;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	format.fmt.pix.bytesperline = 3 * frame_width;
	format.fmt.pix.sizeimage = format.fmt.pix.bytesperline * frame_height;
	format.fmt.pix.width = frame_width;
	format.fmt.pix.height = frame_height;
	if (frame_width > best_format.fmt.pix.width)
		best_format = format;
}
			}
		}
		camera.best_format = best_format;
		camera.name = a_sprintf(
			"%s %s (up to %" PRIu32 "x%" PRIu32 ")", (const char *)cap.card, (const char *)input.name,
			best_format.fmt.pix.width, best_format.fmt.pix.height
		);
		camera.input_idx = input_idx;
		camera.dev_path = strdup(dev_path);
		arr_add(*cameras, camera);
	}
}

int main() {
	if (sodium_init() < 0) {
		fprintf(stderr, "couldn't initialize libsodium");
		return EXIT_FAILURE;
	}
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // if this program is sent a SIGTERM/SIGINT, don't turn it into a quit event
	if (access("/dev", R_OK) != 0) {
		fprintf(stderr, "Can't access /dev");
		return EXIT_FAILURE;
	}
	SDL_Init(SDL_INIT_EVERYTHING);
	SDL_Window *window = SDL_CreateWindow("camlet", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
	int gl_version_major = 3, gl_version_minor = 0;
	#if DEBUG
		gl_version_major = 4;
		gl_version_minor = 3;
	#endif
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl_version_major);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl_version_minor);
	SDL_GLContext glctx = SDL_GL_CreateContext(window);
	if (!glctx) {
		printf("couldn't create GL context: %s\n", SDL_GetError());
	}
	SDL_GL_SetSwapInterval(1); // vsync
	#define gl_get_proc(upper, lower) gl_##lower = (PFNGL##upper##PROC)SDL_GL_GetProcAddress("gl" #lower);
	gl_for_each_proc(gl_get_proc);
	#if DEBUG
	{
		GLint flags = 0;
		gl_GetIntegerv(GL_CONTEXT_FLAGS, &flags);
		gl_Enable(GL_DEBUG_OUTPUT);
		gl_Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
			// set up debug message callback
			gl_DebugMessageCallback(gl_message_callback, NULL);
			gl_DebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
		}
	}
	#endif
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double last_time = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
	GLuint texture = 0;
	gl_GenTextures(1, &texture);
	gl_BindTexture(GL_TEXTURE_2D, texture);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const char *vshader_code = "attribute vec2 v_pos;\n\
attribute vec2 v_tex_coord;\n\
out vec2 tex_coord;\n\
void main() {\n\
	tex_coord = vec2(v_tex_coord.x, 1.0 - v_tex_coord.y);\n\
	gl_Position = vec4(v_pos, 0.0, 1.0);\n\
}\n\
";
	const char *fshader_code = "in vec4 color;\n\
in vec2 tex_coord;\n\
uniform sampler2D u_sampler;\n\
void main() {\n\
	vec4 tex_color = texture2D(u_sampler, tex_coord);\n\
	gl_FragColor = vec4(tex_color.xyz, 1.0);\n\
}\n\
";
	char err[256] = {0};
	GLuint program = gl_compile_and_link_shaders(err, vshader_code, fshader_code);
	if (program == 0) return EXIT_FAILURE;
	GLuint vbo = 0, vao = 0;
	gl_GenBuffers(1, &vbo);
	gl_GenVertexArrays(1, &vao);
	typedef struct {
		float pos[2];
		float tex_coord[2];
	} Vertex;
	typedef struct {
		Vertex v0;
		Vertex v1;
		Vertex v2;
	} Triangle;
	Triangle triangles[2] = {
		{ {{-1, -1}, {0, 0}}, {{1, 1}, {1, 1}}, {{-1, 1}, {0, 1}} },
		{ {{-1, -1}, {0, 0}}, {{1, -1}, {1, 0}}, {{1, 1}, {1, 1}} },
	};
	int ntriangles = sizeof triangles / sizeof triangles[0];
	GLuint u_sampler = gl_GetUniformLocation(program, "u_sampler");
	GLint v_pos = gl_GetAttribLocation(program, "v_pos");
	GLint v_tex_coord = gl_GetAttribLocation(program, "v_tex_coord");
	gl_BindBuffer(GL_ARRAY_BUFFER, vbo);
	gl_BindVertexArray(vao);
	gl_BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ntriangles * sizeof(Triangle)), triangles, GL_STATIC_DRAW);
	gl_VertexAttribPointer(v_pos, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, pos));
	gl_EnableVertexAttribArray(v_pos);
	gl_VertexAttribPointer(v_tex_coord, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, tex_coord));
	gl_EnableVertexAttribArray(v_tex_coord);
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
	Camera *cameras = NULL;
	{
		struct udev_enumerate *enumerate = udev_enumerate_new(udev);
		udev_enumerate_add_match_subsystem(enumerate, "video4linux");
		udev_enumerate_add_match_subsystem(enumerate, "usb");
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
				int fd = v4l2_open(devnode, O_RDWR | O_NONBLOCK);
				if (fd < 0) {
					perror("v4l2_open");
					goto cont;
				}
				get_cameras_from_device(devnode, serial, fd, &cameras);
				v4l2_close(fd);
			}
			cont:
			udev_device_unref(dev);
		}
		udev_list_entry_foreach(device, devices) {
			struct udev_device *dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(device));
			const char *busnum_str = udev_device_get_sysattr_value(dev, "busnum");
			if (!busnum_str) continue;
			const char *devnum_str = udev_device_get_sysattr_value(dev, "devnum");
			if (!devnum_str) continue;
			const char *devpath_str = udev_device_get_sysattr_value(dev, "devpath");
			if (!devpath_str) continue;
			const char *serial = udev_device_get_sysattr_value(dev, "serial");
			if (!serial || !*serial) continue;
			int busnum = atoi(busnum_str);
			int devnum = atoi(devnum_str);
			int devpath = atoi(devpath_str);
			arr_foreach_ptr(cameras, Camera, camera) {
				// allows us to distinguish between different instances of the exact same model of camera
				if (camera->usb_busnum == busnum && camera->usb_devnum == devnum && camera->usb_devpath == devpath) {
					crypto_generichash_update(&camera->hash_state, (const uint8_t *)serial, strlen(serial) + 1);
				}
			}
			udev_device_unref(dev);
		}
		udev_enumerate_unref(enumerate);
		arr_foreach_ptr(cameras, Camera, camera) {
			memset(camera->hash.hash, 0, sizeof camera->hash.hash);
			crypto_generichash_final(&camera->hash_state, camera->hash.hash, sizeof camera->hash.hash);
		}
		printf("Select a camera:\n");
		for (size_t i = 0; i < arr_len(cameras); i++) {
			Camera *camera =  &cameras[i];
			printf("[%zu] %s ", i, camera->name);
			for (size_t h = 0; h < sizeof camera->hash.hash; h++) {
				printf("%02x", camera->hash.hash[h]);
			}
			printf("\n");
		}
	}
	int choice = 0;
	if (arr_len(cameras) == 0) {
		printf("no cameras\n");
		return EXIT_FAILURE;
	}
	Camera *camera = &cameras[choice];
	int camera_fd = v4l2_open(camera->dev_path, O_RDWR);
	if (camera_fd < 0) {
		perror("v4l2_open");
		return EXIT_FAILURE;
	}
	if (v4l2_ioctl(camera_fd, VIDIOC_S_INPUT, &camera->input_idx) != 0) {
		perror("v4l2_ioctl");
		return EXIT_FAILURE;
	}
	if (v4l2_ioctl(camera_fd, VIDIOC_S_FMT, &camera->best_format) != 0) {
		perror("v4l2_ioctl");
		return EXIT_FAILURE;
	}
	uint32_t frame_width = camera->best_format.fmt.pix.width;
	uint32_t frame_height = camera->best_format.fmt.pix.height;
	uint8_t *buf = calloc(1, camera->best_format.fmt.pix.sizeimage);
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
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) goto quit;
			if (event.type == SDL_KEYDOWN) switch (event.key.keysym.sym) {
			case SDLK_SPACE: {
				time_t t = time(NULL);
				struct tm *tm = localtime(&t);
				char name[256];
				strftime(name, sizeof name, "%Y-%m-%d-%H-%M-%S.jpg", tm);
				stbi_write_jpg(name, frame_width, frame_height, 3, buf, 90);
				} break;
			}
		}
		int window_width = 0, window_height = 0;
		SDL_GetWindowSize(window, &window_width, &window_height);
		gl_Viewport(0, 0, window_width, window_height);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		double t = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
		//printf("%.1fms frame time\n",(t-last_time)*1000);
		last_time = t; (void)last_time;
		gl_ActiveTexture(GL_TEXTURE0);
		gl_BindTexture(GL_TEXTURE_2D, texture);
		for (int align = 8; align >= 1; align >>= 1) {
			if (frame_width % align == 0) {
				gl_PixelStorei(GL_UNPACK_ALIGNMENT, align);
				break;
			}
		}
		if (v4l2_read(camera_fd, buf, camera->best_format.fmt.pix.sizeimage) == camera->best_format.fmt.pix.sizeimage) {
			gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame_width, frame_height, 0, GL_RGB, GL_UNSIGNED_BYTE, buf);
		}
		gl_UseProgram(program);
		gl_BindBuffer(GL_ARRAY_BUFFER, vbo);
		gl_BindVertexArray(vao);
		gl_Uniform1i(u_sampler, 0);
		gl_DrawArrays(GL_TRIANGLES, 0, (GLsizei)(3 * ntriangles));
		gl_BindTexture(GL_TEXTURE_2D, 0);
		SDL_GL_SwapWindow(window);
	}
	quit:
	udev_monitor_unref(udev_monitor);
	udev_unref(udev);
	//debug_save_24bpp_bmp("out.bmp", buf, camera->best_format.fmt.pix.width, camera->best_format.fmt.pix.height);
	v4l2_close(camera_fd);
	SDL_Quit();
	return 0;
}

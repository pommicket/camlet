#ifndef CAMERA_H_
#define CAMERA_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <GL/glcorearb.h>

typedef uint32_t PixelFormat;
typedef struct Camera Camera;
#define HASH_SIZE 16
typedef struct {
	uint8_t hash[HASH_SIZE];
} Hash;

typedef struct {
	int32_t width;
	int32_t height;
	PixelFormat pixfmt;
} PictureFormat;

typedef enum {
	// (default value)
	CAMERA_ACCESS_NOT_SETUP,
	// access camera via mmap streaming
	CAMERA_ACCESS_MMAP,
	// access camera via read calls
	CAMERA_ACCESS_READ,
	// access camera via user-pointer streaming
	CAMERA_ACCESS_USERP,
} CameraAccessMethod;

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
typedef struct {
#define declare_proc(upper, lower) PFNGL##upper##PROC lower;
	gl_for_each_proc(declare_proc)
#undef declare_proc
} GlProcs;

void camera_init(const GlProcs *procs);
bool pix_fmt_supported(uint32_t pixfmt);
int picture_format_cmp_resolution(const PictureFormat *a, const PictureFormat *b);
int picture_format_cmp_qsort(const void *av, const void *bv);
const char *pixfmt_to_string(uint32_t pixfmt);
PictureFormat *camera_get_resolutions_with_pixfmt(Camera *camera, uint32_t pixfmt);
uint32_t *camera_get_pixfmts(Camera *camera);
PictureFormat camera_closest_resolution(Camera *camera, uint32_t pixfmt, int32_t desired_width, int32_t desired_height);
int32_t camera_frame_width(Camera *camera);
int32_t camera_frame_height(Camera *camera);
PictureFormat camera_picture_format(Camera *camera);
void camera_write_jpg(Camera *camera, const char *name, int quality);
bool camera_next_frame(Camera *camera);
void camera_update_gl_texture_2d(Camera *camera);
const char *camera_name(Camera *camera);
uint32_t camera_pixel_format(Camera *camera);
CameraAccessMethod camera_access_method(Camera *camera);
void camera_close(Camera *camera);
void cameras_from_device(const char *dev_path, const char *serial, Camera ***cameras);
void camera_update_hash(Camera *camera, const void *data, size_t len);
bool camera_open(Camera *camera);
Hash camera_hash(Camera *camera);
void camera_hash_str(Camera *camera, char str[HASH_SIZE * 2 + 1]);
bool camera_set_format(Camera *camera, PictureFormat picfmt, CameraAccessMethod access, bool force);
void camera_free(Camera *camera);

#endif

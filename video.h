#ifndef VIDEO_H_
#define VIDEO_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct VideoContext VideoContext;
struct Camera;

VideoContext *video_init(void);
bool video_start(VideoContext *ctx, const char *filename, int32_t width, int32_t height, int fps, int quality);
bool video_is_recording(VideoContext *ctx);
void video_stop(VideoContext *ctx);
bool video_submit_frame(VideoContext *ctx, struct Camera *camera);
void video_quit(VideoContext *ctx);

#endif // VIDEO_H_

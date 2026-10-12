/*
 * nativevideo - tiny video decoder that uses the operating system's own codecs.
 *
 *   Windows : Media Foundation   (nv_mediafoundation.cpp)
 *   macOS   : AVFoundation       (nv_avfoundation.m)
 *   Linux   : GStreamer          (nv_gstreamer.c)
 *
 * Every backend outputs tightly packed RGBA8 frames (width * 4 bytes per row),
 * so the Lua side never has to care which OS it is on.
 *
 * Threading: open, read, seek and close one video from the same thread.
 */
#ifndef NATIVEVIDEO_H
#define NATIVEVIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define NV_API __declspec(dllexport)
#else
#  define NV_API __attribute__((visibility("default")))
#endif

typedef struct nv_video nv_video;

typedef struct nv_info {
    int32_t width;     /* visible frame size in pixels                  */
    int32_t height;
    double  fps;       /* nominal frame rate, 0 if unknown               */
    double  duration;  /* seconds, 0 if unknown                          */
} nv_info;

/* Open a file by absolute UTF-8 path. Returns NULL on failure (see nv_last_error). */
NV_API nv_video*   nv_open(const char* path_utf8);

NV_API void        nv_get_info(nv_video* v, nv_info* out);

/* Decode the next frame into rgba_out (width*height*4 bytes).
 * Returns 1 = frame written, 0 = end of stream, -1 = error.
 * pts_out receives the frame's presentation time in seconds. */
NV_API int         nv_read_frame(nv_video* v, uint8_t* rgba_out, double* pts_out);

/* Jump to roughly `seconds`. Some backends land on the keyframe before the
 * target; the Lua player skips forward to the exact frame. Returns 1 on success. */
NV_API int         nv_seek(nv_video* v, double seconds);

NV_API void        nv_close(nv_video* v);

/* Last error message from any call (not thread safe; for diagnostics). */
NV_API const char* nv_last_error(void);

/* Name of the backend compiled in: "mediafoundation", "avfoundation" or "gstreamer". */
NV_API const char* nv_backend(void);

#ifdef __cplusplus
}
#endif

#endif

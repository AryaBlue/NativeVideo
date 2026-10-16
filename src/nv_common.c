#include "nativevideo.h"
#include "nv_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_error[512] = "";

void nv_set_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

const char* nv_last_error(void) { return g_error; }
void nv_copy_rows(uint8_t* dst, const uint8_t* src, long src_pitch,
                  int width, int height, int bgra)
{
    for (int y = 0; y < height; y++) {
        const uint8_t* s = src + (long)y * src_pitch;
        uint8_t* d = dst + (long)y * width * 4;
        if (bgra) {
            for (int x = 0; x < width; x++, s += 4, d += 4) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            }
        } else {
            memcpy(d, s, (size_t)width * 4);
        }
    }
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void nv_nv12_to_rgba(uint8_t* dst, const uint8_t* yp, long y_pitch,
                     const uint8_t* uvp, long uv_pitch,
                     int width, int height, int bt709, int full_range)
{
    /* Fixed-point (x1024) YUV -> RGB coefficients. */
    int ky, y_off, rv, gu, gv, bu;
    if (full_range) {
        ky = 1024; y_off = 0;
        if (bt709) { rv = 1613; gu = -192; gv = -479; bu = 1900; }
        else       { rv = 1436; gu = -352; gv = -731; bu = 1815; }
    } else {
        ky = 1192; y_off = 16;
        if (bt709) { rv = 1836; gu = -218; gv = -546; bu = 2163; }
        else       { rv = 1634; gu = -401; gv = -832; bu = 2066; }
    }
    for (int j = 0; j < height; j++) {
        const uint8_t* yr = yp + (long)j * y_pitch;
        const uint8_t* uvr = uvp + (long)(j / 2) * uv_pitch;
        uint8_t* d = dst + (long)j * width * 4;
       
        for (int i = 0; i < width; i++, d += 4) {
            int c = (yr[i] - y_off) * ky;
            int u = uvr[(i & ~1)] - 128;
            int v = uvr[(i & ~1) + 1] - 128;
            d[0] = clamp8((c + rv * v + 512) >> 10);
            d[1] = clamp8((c + gu * u + gv * v + 512) >> 10);
            d[2] = clamp8((c + bu * u + 512) >> 10);
            d[3] = 255;
        }
    }
}

#ifndef NV_INTERNAL_H
#define NV_INTERNAL_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nv_set_error(const char* fmt, ...);
void nv_copy_rows(uint8_t* dst, const uint8_t* src, long src_pitch,
                  int width, int height, int bgra);

/* NV12 (Y plane + interleaved UV plane at half resolution) -> RGBA.
 * bt709: 1 = BT.709 colours (HD), 0 = BT.601 (SD). full_range: 1 = 0..255 luma. */
void nv_nv12_to_rgba(uint8_t* dst, const uint8_t* y, long y_pitch,
                     const uint8_t* uv, long uv_pitch,
                     int width, int height, int bt709, int full_range);

#ifdef __cplusplus
}
#endif
#endif

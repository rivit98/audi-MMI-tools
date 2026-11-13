/*
 * imgdec.h - JPEG (baseline/progressive) -> 0xAARRGGBB pixels (stb_image).
 */
#ifndef COMMON_IMGDEC_H
#define COMMON_IMGDEC_H

#include "types.h"

/* Dimensions of a JPEG in data[0..n); 0 if it is none. */
int imgdec_info(const u8 *data, unsigned n, int *w, int *h);

/* Decode into out (malloc()ed, w*h pixels, opaque, stride = w); 0 on
 * success, -1 if it is no JPEG, is
 * larger than max_w x max_h, or memory runs out. The caller frees *out. */
int imgdec_argb(const u8 *data, unsigned n, int max_w, int max_h,
                u32 **out, int *w, int *h);

#endif

/*
 * jpeg.h - baseline/progressive JPEG -> 0xAARRGGBB pixels (stb_image).
 */
#ifndef COMMON_JPEG_H
#define COMMON_JPEG_H

#include <stdint.h>

/* Dimensions of a JPEG in data[0..n); 0 if it is not one. */
int jpeg_info(const uint8_t *data, unsigned n, int *w, int *h);

/* Decode into out (malloc()ed, w*h pixels, opaque, stride = w); 0 on
 * success, -1 if it is not a JPEG, is larger than max_w x max_h, or memory
 * runs out. The caller frees *out. */
int jpeg_decode_argb(const uint8_t *data, unsigned n, int max_w, int max_h,
                     uint32_t **out, int *w, int *h);

#endif

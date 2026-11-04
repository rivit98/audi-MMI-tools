/*
 * imgdec.h - JPEG (baseline/progressive) or PNG -> 0xAARRGGBB pixels
 * (stb_image). The format is taken from the data itself.
 */
#ifndef COMMON_IMGDEC_H
#define COMMON_IMGDEC_H

#include <stdint.h>

/* Dimensions of a JPEG or PNG in data[0..n); 0 if it is neither. */
int imgdec_info(const uint8_t *data, unsigned n, int *w, int *h);

/* Decode into out (malloc()ed, w*h pixels, opaque - a PNG's alpha is
 * dropped, stride = w); 0 on success, -1 if it is no JPEG or PNG, is
 * larger than max_w x max_h, or memory runs out. The caller frees *out. */
int imgdec_argb(const uint8_t *data, unsigned n, int max_w, int max_h,
                uint32_t **out, int *w, int *h);

#endif

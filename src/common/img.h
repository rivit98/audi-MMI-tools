/*
 * img.h - image file output (BMP, PNG) over the vendored stb_image_write.h.
 *
 * The SDK has no zlib or libpng: stb_image_write (Sean Barrett, public
 * domain / MIT) has its own deflate, so nothing else is linked.
 */
#ifndef RIVIT_IMG_H
#define RIVIT_IMG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Write w x h RGBA8888 pixels as a 24-bit BMP (alpha ignored). Rows are
 * bottom-up, which is what glReadPixels delivers. Returns 0 or -1 (errno
 * set when it is known). */
int img_write_bmp_rgba(const char *path, const unsigned char *rgba, int w, int h);

/* Encode w x h pixels of 0xAARRGGBB words (stride in pixels, rows top-down,
 * alpha ignored) as an 8-bit RGB PNG into a malloc'd buffer. Returns 0, or
 * -1 if out of memory. */
int img_png_encode_argb(const uint32_t *px, int stride, int w, int h,
                        unsigned char **out, size_t *len);

/* img_png_encode_argb() written to f. Returns 0 or -1 (errno set). */
int img_png_write_argb(FILE *f, const uint32_t *px, int stride, int w, int h);

#endif

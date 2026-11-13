/*
 * img.c - BMP / PNG output through stb_image_write (see img.h).
 *
 * stb_image_write.h is vendored unchanged (v1.16, nothings/stb commit
 * 1ee679c, public domain / MIT; the licence is at its end). Only its BMP
 * writer and in-memory PNG writer are used.
 */
#include "types.h"
#include <errno.h>
#include <stdlib.h>

#include "img.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   /* its other writers */
#include "stb_image_write.h"
#pragma GCC diagnostic pop

int img_write_bmp_rgba(const char *path, const unsigned char *rgba, int w, int h)
{
	unsigned char *rgb = malloc((usize)w * h * 3), *p = rgb;
	int x, y, ok;

	if (rgb == NULL)
		return -1;
	/* stb takes top-down rows and writes the file bottom-up; ours are
	 * bottom-up already, so hand them over reversed. */
	for (y = h - 1; y >= 0; y--) {
		const unsigned char *src = rgba + (usize)y * w * 4;

		for (x = 0; x < w; x++, src += 4) {
			*p++ = src[0];
			*p++ = src[1];
			*p++ = src[2];
		}
	}
	ok = stbi_write_bmp(path, w, h, 3, rgb);
	free(rgb);
	return ok ? 0 : -1;
}

int img_png_encode_argb(const u32 *px, int stride, int w, int h,
                        unsigned char **out, usize *len)
{
	unsigned char *rgb = malloc((usize)w * h * 3), *p = rgb;
	int x, y, n;

	if (rgb == NULL)
		return -1;
	for (y = 0; y < h; y++) {
		const u32 *row = px + (usize)y * stride;

		for (x = 0; x < w; x++) {
			*p++ = (unsigned char)(row[x] >> 16);
			*p++ = (unsigned char)(row[x] >> 8);
			*p++ = (unsigned char)row[x];
		}
	}
	*out = stbi_write_png_to_mem(rgb, w * 3, w, h, 3, &n);
	free(rgb);
	if (*out == NULL)
		return -1;
	*len = (usize)n;
	return 0;
}

int img_png_write_argb(FILE *f, const u32 *px, int stride, int w, int h)
{
	unsigned char *buf;
	usize len;
	int rc = 0;

	if (img_png_encode_argb(px, stride, w, h, &buf, &len) != 0) {
		errno = ENOMEM;
		return -1;
	}
	if (fwrite(buf, 1, len, f) != len || fflush(f) != 0)
		rc = -1;
	free(buf);
	return rc;
}

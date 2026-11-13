/*
 * imgdec.c - JPEG input through stb_image (see imgdec.h).
 *
 * stb_image.h is vendored unchanged (v2.30, from Debian's libstb-dev
 * 0.0~git20250907.fede005+ds-1, nothings/stb commit fede005; public domain
 * / MIT, the licence is at its end). Only its JPEG decoder is compiled in:
 * no stdio, no HDR/linear float
 * paths (so no libm), no SIMD (SH4; on the host test the same scalar code
 * runs), and no thread-local error string - QNX 6.3.2 has no TLS.
 */
#include "types.h"
#include <stdlib.h>

#include "imgdec.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 4096
/* Its other loaders. Not popped: GCC reports unused static declarations at
 * the end of the file. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"

int imgdec_info(const u8 *data, unsigned n, int *w, int *h)
{
	int comp;

	return stbi_info_from_memory(data, (int)n, w, h, &comp);
}

int imgdec_argb(const u8 *data, unsigned n, int max_w, int max_h,
                u32 **out, int *w, int *h)
{
	unsigned char *rgb;
	u32 *px;
	usize i, count;
	int comp;

	*out = NULL;
	if (!imgdec_info(data, n, w, h) || *w > max_w || *h > max_h)
		return -1;
	rgb = stbi_load_from_memory(data, (int)n, w, h, &comp, 3);
	if (rgb == NULL)
		return -1;
	count = (usize)*w * *h;
	px = malloc(count * 4);
	if (px == NULL) {
		stbi_image_free(rgb);
		return -1;
	}
	for (i = 0; i < count; i++)
		px[i] = 0xFF000000u | (u32)rgb[3 * i] << 16 |
		        (u32)rgb[3 * i + 1] << 8 | rgb[3 * i + 2];
	stbi_image_free(rgb);
	*out = px;
	return 0;
}

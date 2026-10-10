#include "bmp.h"

#include <stdio.h>
#include <stdlib.h>

static void put_le(unsigned char *p, unsigned v, int bytes)
{
	while (bytes-- > 0) {
		*p++ = (unsigned char)v;
		v >>= 8;
	}
}

int bmp_write_rgba(const char *path, const unsigned char *rgba, int w, int h)
{
	int stride = (w * 3 + 3) & ~3;
	unsigned char hdr[54] = { 'B', 'M' };
	unsigned char *row;
	FILE *f;
	int x, y, ok;

	put_le(hdr + 2, 54 + stride * h, 4); /* file size */
	put_le(hdr + 10, 54, 4);             /* pixel data offset */
	put_le(hdr + 14, 40, 4);             /* BITMAPINFOHEADER */
	put_le(hdr + 18, w, 4);
	put_le(hdr + 22, h, 4);              /* positive: bottom-up */
	put_le(hdr + 26, 1, 2);              /* planes */
	put_le(hdr + 28, 24, 2);             /* bits per pixel */
	put_le(hdr + 34, stride * h, 4);

	row = calloc(1, stride);
	f = fopen(path, "wb");
	if (row == NULL || f == NULL) {
		free(row);
		if (f != NULL)
			fclose(f);
		return -1;
	}
	ok = fwrite(hdr, sizeof(hdr), 1, f) == 1;
	for (y = 0; ok && y < h; y++) {
		const unsigned char *src = rgba + (size_t)y * w * 4;

		for (x = 0; x < w; x++) { /* RGBA -> BGR */
			row[x * 3 + 0] = src[x * 4 + 2];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 0];
		}
		ok = fwrite(row, stride, 1, f) == 1;
	}
	free(row);
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

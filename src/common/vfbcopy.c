/*
 * vfbcopy.c - full or changed-span copy into the VFB (see vfbcopy.h).
 */
#include "types.h"
#include <string.h>

#include "vfbcopy.h"

/* Changed pixels of a row: [*x0, *x1). 0 if none. */
static int row_span(const u32 *a, const u32 *b, int w, int *x0, int *x1)
{
	int l = 0, r = w;

	while (l < r && a[l] == b[l])
		l++;
	if (l == r)
		return 0;
	while (a[r - 1] == b[r - 1])
		r--;
	*x0 = l;
	*x1 = r;
	return 1;
}

struct vfb_copied vfb_copy(u8 *dst, unsigned dst_stride, u32 *shadow,
                           const u32 *src, int src_stride, int w, int h, int full)
{
	struct vfb_copied c = { 0, 0 };
	int y, x0, x1;

	if (shadow == NULL)
		full = 1;
	for (y = 0; y < h; y++) {
		const u32 *s = src + y * src_stride;
		u8 *d = dst + (unsigned)y * dst_stride;
		u32 *sh = shadow != NULL ? shadow + y * w : NULL;

		if (full) {
			x0 = 0;
			x1 = w;
		} else if (!row_span(s, sh, w, &x0, &x1)) {
			continue;
		}
		memcpy(d + x0 * 4, s + x0, (usize)(x1 - x0) * 4);
		if (sh != NULL)
			memcpy(sh + x0, s + x0, (usize)(x1 - x0) * 4);
		c.bytes += (unsigned)(x1 - x0) * 4;
		c.rows++;
	}
	return c;
}

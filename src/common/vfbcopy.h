/*
 * vfbcopy.h - copy a RAM canvas into a (slow, CPU-mapped) VFB, all of it
 * or only what changed. Pure: no gf, no layer manager (screen.c owns the
 * surfaces; tests/test_vfbcopy.c checks this on the build host).
 */
#ifndef COMMON_VFBCOPY_H
#define COMMON_VFBCOPY_H

#include <stdint.h>

struct vfb_copied {
	unsigned bytes;   /* written into dst */
	unsigned rows;    /* rows with something written */
};

/*
 * src (w x h ARGB, src_stride pixels a row) -> dst (dst_stride bytes a row).
 * shadow (w x h, w pixels a row, or NULL) is what dst holds now, and is
 * kept in step with it. full (or no shadow): every row whole. Else each
 * row's first to last pixel that differs from the shadow, nothing for an
 * unchanged row.
 */
struct vfb_copied vfb_copy(uint8_t *dst, unsigned dst_stride, uint32_t *shadow,
                           const uint32_t *src, int src_stride, int w, int h, int full);

#endif

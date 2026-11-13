/*
 * test_vfbcopy.c - screen.c's VFB copy (src/common/vfbcopy.c), make test.
 * The VFB here is plain memory with a wider stride than the canvas, as on
 * the car (stride 3200 for 800 pixels, here padded further).
 */
#include "types.h"
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "vfbcopy.h"

#define W  37
#define H  11
#define SS (W + 5)              /* canvas stride, pixels */
#define DS ((W + 3) * 4)        /* VFB stride, bytes */

static u32 src[H * SS], shadow[W * H];
static u8  vfb[H * DS];

static u32 vfb_px(int x, int y)
{
	u32 v;

	memcpy(&v, vfb + y * DS + x * 4, 4);
	return v;
}

/* The VFB shows the canvas, and the shadow matches the VFB. */
static int in_step(void)
{
	int x, y;

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			if (vfb_px(x, y) != src[y * SS + x] || shadow[y * W + x] != src[y * SS + x])
				return 0;
	return 1;
}

/* Nothing outside the w x h area of the VFB is touched. */
static int padding_intact(void)
{
	int x, y;

	for (y = 0; y < H; y++)
		for (x = W * 4; x < DS; x++)
			if (vfb[y * DS + x] != 0xAB)
				return 0;
	return 1;
}

int main(void)
{
	struct vfb_copied c;
	int i, x, y;

	srand(7);
	memset(vfb, 0xAB, sizeof(vfb));
	for (i = 0; i < H * SS; i++)
		src[i] = (u32)rand();

	/* full: every row whole */
	c = vfb_copy(vfb, DS, shadow, src, SS, W, H, 1);
	CHECK_EQ(c.bytes, W * H * 4);
	CHECK_EQ(c.rows, H);
	CHECK(in_step());
	CHECK(padding_intact());

	/* nothing changed: nothing written */
	c = vfb_copy(vfb, DS, shadow, src, SS, W, H, 0);
	CHECK_EQ(c.bytes, 0);
	CHECK_EQ(c.rows, 0);

	/* one pixel */
	src[4 * SS + 9] ^= 1;
	c = vfb_copy(vfb, DS, shadow, src, SS, W, H, 0);
	CHECK_EQ(c.bytes, 4);
	CHECK_EQ(c.rows, 1);
	CHECK(in_step());

	/* first and last pixel of a row: the whole row; plus the corners */
	src[2 * SS] ^= 1;
	src[2 * SS + W - 1] ^= 1;
	src[0] ^= 1;
	src[(H - 1) * SS + W - 1] ^= 1;
	c = vfb_copy(vfb, DS, shadow, src, SS, W, H, 0);
	CHECK_EQ(c.bytes, (W + 1 + 1) * 4);
	CHECK_EQ(c.rows, 3);
	CHECK(in_step());
	CHECK(padding_intact());

	/* a span in the middle of every row */
	for (y = 0; y < H; y++)
		for (x = 10; x < 20; x++)
			src[y * SS + x] += 3;
	c = vfb_copy(vfb, DS, shadow, src, SS, W, H, 0);
	CHECK_EQ(c.bytes, 10 * H * 4);
	CHECK_EQ(c.rows, H);
	CHECK(in_step());

	/* the canvas's padding (stride > w) is never looked at */
	for (y = 0; y < H; y++)
		for (x = W; x < SS; x++)
			src[y * SS + x] ^= 0xFFFFFFFFu;
	c = vfb_copy(vfb, DS, shadow, src, SS, W, H, 0);
	CHECK_EQ(c.bytes, 0);

	/* random edits, many rounds */
	for (i = 0; i < 200; i++) {
		int n = rand() % 20;

		while (n-- > 0)
			src[(rand() % H) * SS + rand() % W] = (u32)rand();
		vfb_copy(vfb, DS, shadow, src, SS, W, H, 0);
		if (!in_step())
			break;
	}
	CHECK(in_step());
	CHECK(padding_intact());

	/* no shadow: full, whatever was asked */
	src[5] ^= 1;
	c = vfb_copy(vfb, DS, NULL, src, SS, W, H, 0);
	CHECK_EQ(c.bytes, W * H * 4);
	CHECK_EQ(vfb_px(5, 0), src[5]);

	CHECK_DONE();
}

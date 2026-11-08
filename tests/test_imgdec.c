/* test_imgdec.c - picture decoding (src/common/imgdec.c): pictures encoded
 * by stb_image_write's JPEG writer decode to the right size and colours;
 * its PNG is refused. */
#include <stdlib.h>

#include "check.h"
#include "imgdec.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

struct mem {
	unsigned char buf[1 << 19];
	unsigned n;
};

static void put(void *ctx, void *data, int size)
{
	struct mem *m = ctx;

	if (m->n + (unsigned)size <= sizeof(m->buf)) {
		memcpy(m->buf + m->n, data, (size_t)size);
		m->n += (unsigned)size;
	}
}

/* Quadrants: red, green / blue, white. */
static const unsigned char quad[4][3] = {
	{ 230, 20, 20 }, { 20, 230, 20 }, { 20, 20, 230 }, { 240, 240, 240 },
};

static struct mem jpg, png;

/* comp 3 = RGB, 4 = RGBA (alpha 0x80: the decoder must drop it) */
static unsigned char *quads(int w, int h, int comp)
{
	unsigned char *px = malloc((size_t)w * h * comp);
	int x, y, q;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			q = (y >= h / 2) * 2 + (x >= w / 2);
			memcpy(px + ((size_t)y * w + x) * comp, quad[q], 3);
			if (comp == 4)
				px[((size_t)y * w + x) * 4 + 3] = 0x80;
		}
	return px;
}

static void encode(int w, int h)
{
	unsigned char *rgb = quads(w, h, 3);

	jpg.n = 0;
	CHECK(stbi_write_jpg_to_func(put, &jpg, w, h, 3, rgb, 80));
	free(rgb);
}

static void encode_png(int w, int h, int comp)
{
	unsigned char *px = quads(w, h, comp);

	png.n = 0;
	CHECK(stbi_write_png_to_func(put, &png, w, h, comp, px, w * comp));
	CHECK(png.n > 0 && png.n < sizeof(png.buf));
	free(px);
}

static int near(uint32_t px, const unsigned char *rgb)
{
	int r = (int)(px >> 16 & 0xff), g = (int)(px >> 8 & 0xff), b = (int)(px & 0xff);

	return abs(r - rgb[0]) < 24 && abs(g - rgb[1]) < 24 && abs(b - rgb[2]) < 24;
}

static void test_decode(void)
{
	uint32_t *px;
	int w, h;

	encode(64, 48);
	CHECK(imgdec_info(jpg.buf, jpg.n, &w, &h));
	CHECK_EQ(w, 64);
	CHECK_EQ(h, 48);
	CHECK_EQ(imgdec_argb(jpg.buf, jpg.n, 800, 480, &px, &w, &h), 0);
	CHECK_EQ(w, 64);
	CHECK_EQ(h, 48);
	CHECK_EQ(px[0] >> 24, 0xff);                 /* opaque */
	CHECK(near(px[8 * 64 + 8], quad[0]));
	CHECK(near(px[8 * 64 + 56], quad[1]));
	CHECK(near(px[40 * 64 + 8], quad[2]));
	CHECK(near(px[40 * 64 + 56], quad[3]));
	free(px);
}

static void test_portrait(void)
{
	uint32_t *px;
	int w, h;

	encode(222, 480);                            /* a phone in portrait */
	CHECK_EQ(imgdec_argb(jpg.buf, jpg.n, 400, 480, &px, &w, &h), 0);
	CHECK_EQ(w, 222);
	CHECK_EQ(h, 480);
	CHECK(near(px[400 * 222 + 200], quad[3]));
	free(px);
}

/* PNG is not taken */
static void test_png_refused(void)
{
	uint32_t *px = (uint32_t *)1;
	int w, h;

	encode_png(64, 48, 3);
	CHECK(!imgdec_info(png.buf, png.n, &w, &h));
	CHECK_EQ(imgdec_argb(png.buf, png.n, 800, 480, &px, &w, &h), -1);
	CHECK(px == NULL);
}

static void test_refusals(void)
{
	static const unsigned char junk[] = "not a jpeg at all";
	uint32_t *px = (uint32_t *)1;
	int w, h;

	encode(64, 48);
	CHECK_EQ(imgdec_argb(jpg.buf, jpg.n, 63, 480, &px, &w, &h), -1);
	CHECK(px == NULL);
	CHECK_EQ(imgdec_argb(jpg.buf, jpg.n, 800, 47, &px, &w, &h), -1);
	CHECK_EQ(imgdec_argb(junk, sizeof(junk), 800, 480, &px, &w, &h), -1);
	CHECK_EQ(imgdec_argb(jpg.buf, jpg.n / 2, 800, 480, &px, &w, &h) == 0 ||
	         px == NULL, 1);                     /* truncated: no crash */
	free(px);
}

int main(void)
{
	test_decode();
	test_portrait();
	test_png_refused();
	test_refusals();
	CHECK_DONE();
}

/*
 * test_png.c - the PNG output, common/img.c over stb_image_write
 * (make test).
 *
 * Encodes a set of canvases (flat colour, noise, gradients, text-like
 * patterns, tiny and odd sizes) and
 * writes each as <dir>/<name>.png plus its expected pixels as <name>.rgb
 * (w, h as two big-endian u32, then RGB). tests/check_png.py decodes the
 * PNGs with Python's zlib and compares. Here: only the PNG structure.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "check.h"
#include "img.h"
#include "text.h"

static const char *dir = "build/test_png.d";

static uint32_t rng = 12345;

static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void save(const char *name, const struct canvas *c)
{
	char path[256];
	unsigned char *buf, hdr[8];
	size_t len;
	FILE *f;
	int x, y;

	CHECK_EQ(img_png_encode_argb(c->px, c->stride, c->w, c->h, &buf, &len), 0);
	CHECK(len > 8 + 25 + 12 + 12);
	CHECK(memcmp(buf, "\x89PNG\r\n\x1a\n", 8) == 0);
	CHECK(memcmp(buf + 12, "IHDR", 4) == 0);
	CHECK_EQ(be32(buf + 16), c->w);
	CHECK_EQ(be32(buf + 20), c->h);
	CHECK(memcmp(buf + len - 8, "IEND", 4) == 0);
	CHECK_EQ(be32(buf + 33), len - 33 - 12 - 12);   /* one IDAT, all of the rest */
	printf("  %-10s %4dx%-4d %8zu bytes (raw %d)\n", name, c->w, c->h, len,
	       (c->w * 3 + 1) * c->h);

	snprintf(path, sizeof(path), "%s/%s.png", dir, name);
	f = fopen(path, "wb");
	CHECK(f != NULL);
	CHECK_EQ(img_png_write_argb(f, c->px, c->stride, c->w, c->h), 0);
	fclose(f);
	free(buf);

	snprintf(path, sizeof(path), "%s/%s.rgb", dir, name);
	f = fopen(path, "wb");
	CHECK(f != NULL);
	hdr[0] = hdr[1] = hdr[4] = hdr[5] = 0;
	hdr[2] = (unsigned char)(c->w >> 8); hdr[3] = (unsigned char)c->w;
	hdr[6] = (unsigned char)(c->h >> 8); hdr[7] = (unsigned char)c->h;
	fwrite(hdr, 1, 8, f);
	for (y = 0; y < c->h; y++)
		for (x = 0; x < c->w; x++) {
			uint32_t p = c->px[y * c->stride + x];
			unsigned char rgb[3] = { p >> 16, p >> 8, p };

			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

static struct canvas make(int w, int h)
{
	struct canvas c = { calloc((size_t)w * h, 4), w, h, w };

	CHECK(c.px != NULL);
	return c;
}

int main(void)
{
	struct canvas c;
	unsigned char *buf;
	size_t len;
	int x, y;

	mkdir("build", 0755);
	mkdir(dir, 0755);

	c = make(800, 480);
	canvas_fill(&c, 0, 0, 800, 480, 0xFF101820);
	CHECK_EQ(img_png_encode_argb(c.px, c.stride, c.w, c.h, &buf, &len), 0);
	CHECK(len < 20000);                     /* a flat screen is small */
	free(buf);
	save("flat", &c);

	/* a status-screen-like picture: bars, blocks, "glyph" dots */
	canvas_fill(&c, 0, 0, 800, 6, 0xFF5AC8FA);
	canvas_fill(&c, 0, 436, 800, 44, 0xFF1C2836);
	for (y = 20; y < 400; y++)
		for (x = 24; x < 776; x++)
			if ((rnd() & 7) == 0 && (y / 16) % 2 == 0)
				c.px[y * 800 + x] = 0xFFE8E8E8;
	save("screen", &c);

	for (y = 0; y < 480; y++)                     /* no matches at all */
		for (x = 0; x < 800; x++)
			c.px[y * 800 + x] = rnd();
	save("noise", &c);
	free(c.px);

	c = make(300, 200);
	for (y = 0; y < 200; y++)
		for (x = 0; x < 300; x++)
			c.px[y * 300 + x] = (uint32_t)(x << 16 | y << 8 | ((x ^ y) & 0xFF));
	save("gradient", &c);
	for (x = 0; x < 300 * 200; x++) {             /* matches of many lengths */
		int back = 1 + (int)(rnd() % 3);

		c.px[x] = rnd() % 7 == 0 || x < back ? rnd() : c.px[x - back];
	}
	save("runs", &c);
	free(c.px);

	c = make(1, 1);
	c.px[0] = 0xFFABCDEF;
	save("one", &c);
	free(c.px);

	c = make(3000, 3);                            /* long rows, short runs */
	for (x = 0; x < c.w * c.h; x++)
		c.px[x] = (x / 5) % 3 ? 0xFF00FF00 : 0xFF0000FF;
	save("wide", &c);
	free(c.px);

	c = make(11000, 2);                           /* the pixel above is outside */
	for (x = 0; x < c.w * c.h; x++)               /* the 32 KB window */
		c.px[x] = (x % 11000 * 7919u) % 251 * 0x010101u;
	save("long", &c);
	free(c.px);

	CHECK_DONE();
}

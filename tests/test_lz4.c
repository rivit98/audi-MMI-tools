/*
 * test_lz4.c - src/hello6/lz4mini against the reference liblz4: every
 * block we make must decode with LZ4_decompress_safe, every block liblz4
 * makes must decode with ours, and malformed input must be rejected.
 */
#include <lz4.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "lz4mini.h"

static uint32_t rng = 12345;

static unsigned rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 16;
}

/* Both directions on one buffer. */
static void roundtrip(const char *name, const uint8_t *src, int n)
{
	int cap = LZ4MINI_BOUND(n), rcap = LZ4_compressBound(n);
	uint8_t *c = malloc((size_t)cap + 1), *d = malloc((size_t)n + 1);
	uint8_t *rc = malloc((size_t)rcap + 1);
	int cn, dn, rn;

	cn = lz4mini_compress(src, n, c, cap);
	CHECK(cn > 0 || n == 0);
	dn = LZ4_decompress_safe((const char *)c, (char *)d, cn, n);
	CHECK_EQ(dn, n);
	CHECK(dn != n || memcmp(d, src, (size_t)n) == 0);
	dn = lz4mini_decompress(c, cn, d, n);
	CHECK_EQ(dn, n);
	CHECK(dn != n || memcmp(d, src, (size_t)n) == 0);

	rn = LZ4_compress_default((const char *)src, (char *)rc, n, rcap);
	CHECK(rn > 0);
	memset(d, 0xAA, (size_t)n);
	dn = lz4mini_decompress(rc, rn, d, n);
	CHECK_EQ(dn, n);
	CHECK(dn != n || memcmp(d, src, (size_t)n) == 0);

	/* too small an output buffer is an error, not an overflow */
	if (n > 0)
		CHECK_EQ(lz4mini_decompress(rc, rn, d, n - 1), -1);
	if (strcmp(name, "short") != 0)
		printf("  %-12s %7d -> ours %7d, liblz4 %7d\n", name, n, cn, rn);
	free(c);
	free(d);
	free(rc);
}

int main(void)
{
	static uint8_t buf[800 * 480 * 2];
	uint8_t c[64];
	int i, n;

	printf("test_lz4:\n");
	roundtrip("empty", buf, 0);
	buf[0] = 7;
	roundtrip("1 byte", buf, 1);
	for (n = 2; n <= 40; n++) {          /* around the MFLIMIT / LASTLITERALS edges */
		for (i = 0; i < n; i++)
			buf[i] = (uint8_t)(i % 3);
		roundtrip("short", buf, n);
	}
	memset(buf, 0, 65536 + 300);
	roundtrip("zeros", buf, 65536 + 300);
	for (i = 0; i < 100000; i++)
		buf[i] = (uint8_t)rnd();
	roundtrip("random", buf, 100000);
	for (i = 0; i < (int)sizeof(buf); i++)  /* UI-like: long runs + repeats */
		buf[i] = (uint8_t)((i / 1600) % 7 == 0 ? rnd() : (unsigned)(i / 37) % 5);
	roundtrip("frame", buf, (int)sizeof(buf));
	for (i = 0; i < 70000; i++)          /* matches farther than 64 KB back */
		buf[i] = (uint8_t)(i < 1000 ? (unsigned)i * 7 : rnd());
	memcpy(buf + 69000, buf, 1000);
	roundtrip("far", buf, 70000);

	/* malformed input */
	CHECK_EQ(lz4mini_decompress((const uint8_t *)"\xf0", 1, buf, 100), -1); /* runs out */
	c[0] = 0x10; c[1] = 'a'; c[2] = 5; c[3] = 0;                         /* offset > output */
	CHECK_EQ(lz4mini_decompress(c, 4, buf, 100), -1);
	c[0] = 0x10; c[1] = 'a'; c[2] = 0; c[3] = 0;                         /* offset 0 */
	CHECK_EQ(lz4mini_decompress(c, 4, buf, 100), -1);
	c[0] = 0x11; c[1] = 'a'; c[2] = 1; c[3] = 0; c[4] = 0x10; c[5] = 'b'; /* "a" + 5x"a" + "b" */
	CHECK_EQ(lz4mini_decompress(c, 6, buf, 100), 7);
	CHECK(memcmp(buf, "aaaaaab", 7) == 0);

	CHECK_DONE();
}

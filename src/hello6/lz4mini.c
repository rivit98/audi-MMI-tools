#include "lz4mini.h"

#include <string.h>

/* LZ4 block rules: the last 5 bytes are always literals (LASTLITERALS) and
 * no match starts in the last 12 bytes (MFLIMIT). */
#define MIN_MATCH     4
#define LAST_LITERALS 5
#define MF_LIMIT      12
#define MAX_OFFSET    65535
#define HASH_LOG      12

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static unsigned hash4(uint32_t v)
{
	return (v * 2654435761u) >> (32 - HASH_LOG);
}

/* The 255-run extension of a length that did not fit the 4-bit token
 * field. Returns the new op, or -1 if it does not fit. */
static int put_len(uint8_t *dst, int op, int cap, int len)
{
	for (; len >= 255; len -= 255) {
		if (op >= cap)
			return -1;
		dst[op++] = 255;
	}
	if (op >= cap)
		return -1;
	dst[op++] = (uint8_t)len;
	return op;
}

/* One sequence: lit literals from src+anchor, then (if mlen) a match of
 * mlen bytes at distance off. Returns the new op or -1. */
static int put_seq(uint8_t *dst, int op, int cap, const uint8_t *lits, int lit,
                   int off, int mlen)
{
	int tok = op++, ml = mlen - MIN_MATCH;

	if (tok >= cap)
		return -1;
	dst[tok] = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
	if (lit >= 15 && (op = put_len(dst, op, cap, lit - 15)) < 0)
		return -1;
	if (lit > cap - op)
		return -1;
	memcpy(dst + op, lits, (size_t)lit);
	op += lit;
	if (mlen == 0)
		return op;

	if (2 > cap - op)
		return -1;
	dst[op++] = (uint8_t)off;
	dst[op++] = (uint8_t)(off >> 8);
	dst[tok] |= (uint8_t)(ml >= 15 ? 15 : ml);
	if (ml >= 15 && (op = put_len(dst, op, cap, ml - 15)) < 0)
		return -1;
	return op;
}

int lz4mini_compress(const uint8_t *src, int n, uint8_t *dst, int cap)
{
	int table[1 << HASH_LOG];
	int ip = 0, anchor = 0, op = 0, i;

	if (n < 0)
		return -1;
	for (i = 0; i < (1 << HASH_LOG); i++)
		table[i] = -1;

	while (n > MF_LIMIT && ip < n - MF_LIMIT) {
		uint32_t v = rd32(src + ip);
		unsigned h = hash4(v);
		int ref = table[h], len;

		table[h] = ip;
		if (ref < 0 || ip - ref > MAX_OFFSET || rd32(src + ref) != v) {
			ip++;
			continue;
		}
		/* Extend backwards over pending literals, forwards up to the
		 * last-literals boundary. */
		while (ip > anchor && ref > 0 && src[ip - 1] == src[ref - 1]) {
			ip--;
			ref--;
		}
		len = MIN_MATCH;
		while (ip + len < n - LAST_LITERALS && src[ip + len] == src[ref + len])
			len++;

		op = put_seq(dst, op, cap, src + anchor, ip - anchor, ip - ref, len);
		if (op < 0)
			return -1;
		ip += len;
		anchor = ip;
	}
	op = put_seq(dst, op, cap, src + anchor, n - anchor, 0, 0);
	return op;
}

/* Reads a 255-run length extension. Returns the added length, or -1. */
static int get_len(const uint8_t *src, int n, int *ip)
{
	int len = 0;
	unsigned b;

	do {
		if (*ip >= n)
			return -1;
		b = src[(*ip)++];
		len += (int)b;
		if (len > n * 255 + 255)   /* longer than any valid block allows */
			return -1;
	} while (b == 255);
	return len;
}

int lz4mini_decompress(const uint8_t *src, int n, uint8_t *dst, int cap)
{
	int ip = 0, op = 0;

	if (n <= 0)
		return n == 0 ? 0 : -1;
	for (;;) {
		unsigned tok;
		int lit, off, ml, i, ext;

		if (ip >= n)
			return -1;            /* a block ends with a literal run */
		tok = src[ip++];
		lit = (int)(tok >> 4);
		if (lit == 15) {
			if ((ext = get_len(src, n, &ip)) < 0)
				return -1;
			lit += ext;
		}
		if (lit > n - ip || lit > cap - op)
			return -1;
		memcpy(dst + op, src + ip, (size_t)lit);
		ip += lit;
		op += lit;
		if (ip == n)
			return op;            /* last sequence: literals only */

		if (n - ip < 2)
			return -1;
		off = src[ip] | src[ip + 1] << 8;
		ip += 2;
		if (off == 0 || off > op)
			return -1;
		ml = (int)(tok & 15);
		if (ml == 15) {
			if ((ext = get_len(src, n, &ip)) < 0)
				return -1;
			ml += ext;
		}
		ml += MIN_MATCH;
		if (ml > cap - op)
			return -1;
		if (off >= ml) {
			memcpy(dst + op, dst + op - off, (size_t)ml);
		} else {
			for (i = 0; i < ml; i++)          /* overlapping: byte order matters */
				dst[op + i] = dst[op + i - off];
		}
		op += ml;
	}
}

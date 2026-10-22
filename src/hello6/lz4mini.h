/*
 * lz4mini.h - LZ4 block format (no frame header), compress + decompress.
 *
 * The same block format as liblz4's LZ4_compress_default() /
 * LZ4_decompress_safe() and lz4-java's LZ4Compressor, so a phone can send
 * what this decodes (checked against liblz4 by tests/test_lz4.c). Greedy
 * single-probe matcher: fast and simple rather than the best ratio.
 *
 * Byte loads only: the SH4 faults on unaligned 32-bit access.
 */
#ifndef HELLO6_LZ4MINI_H
#define HELLO6_LZ4MINI_H

#include <stdint.h>

/* Worst-case compressed size of n input bytes (incompressible data). */
#define LZ4MINI_BOUND(n) ((n) + (n) / 255 + 16)

/* Compress src[0..n) into dst (cap bytes). Returns the compressed size, or
 * -1 if dst is too small (cap >= LZ4MINI_BOUND(n) always suffices). */
int lz4mini_compress(const uint8_t *src, int n, uint8_t *dst, int cap);

/* Decompress one block. Returns the decompressed size, or -1 if the input
 * is malformed or would overflow dst. Never reads or writes out of bounds. */
int lz4mini_decompress(const uint8_t *src, int n, uint8_t *dst, int cap);

#endif

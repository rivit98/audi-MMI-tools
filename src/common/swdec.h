/*
 * swdec.h - software H.264 Baseline decoding on the SH4 (h264bsd, Android's
 * decoder, Apache-2.0, fetched into third_party/ by the Makefile).
 *
 * One access unit in, at most one picture out, with no frame of delay:
 * swdec_decode() appends an access unit delimiter to its copy of the input,
 * which tells h264bsd the picture is complete. Tested against ffmpeg on the
 * host (tests/test_swdec.cpp).
 */
#ifndef COMMON_SWDEC_H
#define COMMON_SWDEC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct swdec;

struct swdec_pic {
	const u8 *i420;     /* Y, then U, then V, coded size; valid until the next call */
	int width, height;  /* coded size (multiples of 16) */
	int crop_left, crop_top, crop_w, crop_h;   /* the visible part */
	int idr;
	int error_mbs;      /* concealed macroblocks */
};

/* NULL if out of memory. */
struct swdec *swdec_open(void);
void swdec_close(struct swdec *d);

/* Decodes one Annex-B access unit (SPS/PPS may come with it). Returns 1 with
 * *pic filled, 0 if no picture came out (headers only), -1 on an error the
 * decoder can't conceal (the next IDR recovers). */
int swdec_decode(struct swdec *d, const u8 *au, usize n, struct swdec_pic *pic);

/* The visible part of pic in RGB, BT.601 limited range, into out (stride in
 * pixels, alpha 0xff):
 *   bgra    0xAARRGGBB words (bytes B, G, R, A: the VFB's BGRA8888)
 *   rgba    0xAABBGGRR words (bytes R, G, B, A: GL_RGBA / GL_UNSIGNED_BYTE)
 *   rgb565  R in the top 5 bits (GL_UNSIGNED_SHORT_5_6_5, gf PACK_RGB565) */
void swdec_to_bgra(const struct swdec_pic *pic, u32 *out, int stride);
void swdec_to_rgba(const struct swdec_pic *pic, u32 *out, int stride);
void swdec_to_rgb565(const struct swdec_pic *pic, u16 *out, int stride);

/* One pixel of the visible part (x, y from its top left), as swdec_to_bgra
 * makes it: what a read-back of the screen should find there. */
u32 swdec_pixel_bgra(const struct swdec_pic *pic, int x, int y);

/* Largest difference of the three colour bytes of two 0x..RRGGBB words
 * (alpha ignored). */
int swdec_rgb_diff(u32 a, u32 b);

#ifdef __cplusplus
}
#endif

#endif

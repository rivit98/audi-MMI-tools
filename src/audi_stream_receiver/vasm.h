/*
 * vasm.h - reassembles one VIDEO_START + VIDEO_DATA... sequence (proto.h)
 * into a complete picture. Pure: no threads, no USB; the link owns one and
 * feeds it from its frame callback.
 *
 * One picture is assembled at a time. A VIDEO_START while one is still
 * incomplete drops that one (the phone gave up on it); DATA for any other
 * id, or with no picture started, is ignored.
 */
#ifndef AUDI_STREAM_VASM_H
#define AUDI_STREAM_VASM_H

#include <stdint.h>

/* A complete picture; data is malloc()ed and belongs to whoever holds it. */
struct vpic {
	uint8_t *data;
	uint32_t size;
	unsigned id, w, h, format;          /* format: PROTO_FMT_* */
	unsigned phone_ms;                  /* the phone's scale + encode, 0 = not sent */
	unsigned rx_start_us, rx_done_us;   /* log_us(), set by the link (perf) */
};

struct vasm {
	uint32_t    max_bytes;
	int         active;      /* a picture is being assembled */
	struct vpic cur;
	uint32_t    have;        /* bytes of cur received */
};

enum {
	VASM_OK,         /* accepted, picture not complete yet */
	VASM_DONE,       /* picture complete: vasm_take() it */
	VASM_IGNORED,    /* DATA for no picture / another id, or a short frame */
	VASM_BAD,        /* picture refused or broken (id in *bad_id) */
};

void vasm_init(struct vasm *a, uint32_t max_bytes);
void vasm_free(struct vasm *a);        /* drops an incomplete picture */

/* VIDEO_START payload. *dropped_id: id of an incomplete picture this one
 * replaced, or -1. Returns VASM_OK, or VASM_BAD (size 0 / over max_bytes,
 * unknown format, zero width or height, out of memory) with *bad_id set. */
int vasm_start(struct vasm *a, const uint8_t *p, unsigned n, int *dropped_id,
               int *bad_id);

/* VIDEO_DATA payload. VASM_BAD = more bytes than announced (dropped). */
int vasm_data(struct vasm *a, const uint8_t *p, unsigned n, int *bad_id);

/* After VASM_DONE: hand the picture over (out->data is now the caller's). */
void vasm_take(struct vasm *a, struct vpic *out);

#endif

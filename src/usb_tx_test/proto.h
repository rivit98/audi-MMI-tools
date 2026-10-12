/*
 * proto.h - usb_tx_test wire protocol, shared with the Android sender
 * (android/app/src/main/java/rivit/usbtx/Proto.kt must match).
 *
 * Every message on the AOA bulk pipes is one frame:
 *
 *   type u8 | len u16 LE | payload[len]          (len <= PROTO_MAX_PAYLOAD)
 *
 *   HELLO       0x01  both   "who I am" (UTF-8). Sent by each side when its
 *                            session starts; the peer answers HELLO_REPLY.
 *   TEXT        0x02  phone  seq u16 LE + UTF-8 text
 *   ACK         0x03  MMI    seq u16 LE of the TEXT it received
 *   HELLO_REPLY 0x04  both   "who I am", answer to HELLO (never answered)
 *
 * Frames may be split or merged arbitrarily across USB transfers; the
 * decoder reassembles them. A frame announcing more than PROTO_MAX_PAYLOAD
 * is skipped (its bytes are counted off) so the stream stays in sync.
 */
#ifndef USBTX_PROTO_H
#define USBTX_PROTO_H

#include <stdint.h>

#define PROTO_HELLO        0x01
#define PROTO_TEXT         0x02
#define PROTO_ACK          0x03
#define PROTO_HELLO_REPLY  0x04

#define PROTO_HDR          3
#define PROTO_MAX_PAYLOAD  2048
#define PROTO_MAX_FRAME    (PROTO_HDR + PROTO_MAX_PAYLOAD)

typedef void (*proto_frame_cb)(uint8_t type, const uint8_t *payload,
                               unsigned len, void *arg);

struct proto_dec {
	uint8_t  buf[PROTO_MAX_FRAME];
	unsigned have;      /* bytes of the current frame in buf */
	unsigned skip;      /* payload bytes of an oversized frame still to drop */
	unsigned oversized; /* frames dropped for being too big */
};

void proto_dec_init(struct proto_dec *d);

/* Feed n received bytes; cb runs once per complete frame, in order. */
void proto_dec_feed(struct proto_dec *d, const uint8_t *data, unsigned n,
                    proto_frame_cb cb, void *arg);

/* Encode one frame into out[cap]. Returns its size, or 0 if len is over
 * PROTO_MAX_PAYLOAD or the frame does not fit in cap. */
unsigned proto_encode(uint8_t *out, unsigned cap, uint8_t type,
                      const void *payload, unsigned len);

static inline uint16_t proto_get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

#endif

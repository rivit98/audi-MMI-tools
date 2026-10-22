/*
 * proto.h - usb_tx_test wire protocol, shared with the Android sender
 * (android/app/src/main/java/mmi3g/usbtx/Proto.kt must match).
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
 * Video (2 fps-class screen mirroring, one JPEG per picture):
 *
 *   VIDEO_CAPS  0x05  MMI    land_w u16, land_h u16, port_w u16, port_h u16,
 *                            max_bytes u32, formats u8 (bit 0 = JPEG). The
 *                            boxes a landscape / portrait picture has to fit
 *                            in. Sent after HELLO and with HELLO_REPLY; a
 *                            phone streams only to an MMI that sent it.
 *   VIDEO_START 0x06  phone  id u16, w u16, h u16, format u8 (1 = JPEG),
 *                            size u32: a picture of size bytes follows ...
 *   VIDEO_DATA  0x07  phone  id u16 + the next bytes of it, in order
 *   VIDEO_ACK   0x08  MMI    id u16, status u8 (VIDEO_*), decode_ms u16:
 *                            the picture is on screen (or was dropped). The
 *                            phone keeps one picture in flight.
 *   VIDEO_STOP  0x09  phone  (empty) streaming ended, back to text only
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
#define PROTO_VIDEO_CAPS   0x05
#define PROTO_VIDEO_START  0x06
#define PROTO_VIDEO_DATA   0x07
#define PROTO_VIDEO_ACK    0x08
#define PROTO_VIDEO_STOP   0x09

#define PROTO_FMT_JPEG     1

/* VIDEO_ACK status */
#define PROTO_VIDEO_SHOWN    0
#define PROTO_VIDEO_BAD      1   /* not decodable, or bigger than its box */
#define PROTO_VIDEO_DROPPED  2   /* superseded or cut short before shown */

#define PROTO_VIDEO_CAPS_LEN  13
#define PROTO_VIDEO_START_LEN 11
#define PROTO_VIDEO_ACK_LEN   5

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

static inline uint32_t proto_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline void proto_put_u16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8 & 0xff);
}

static inline void proto_put_u32(uint8_t *p, uint32_t v)
{
	proto_put_u16(p, v & 0xffff);
	proto_put_u16(p + 2, v >> 16);
}

#endif

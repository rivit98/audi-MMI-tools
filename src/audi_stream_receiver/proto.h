/*
 * proto.h - audi_stream_receiver wire protocol, shared with the Android sender
 * (android/app/src/main/java/mmi3g/audistreaming/Proto.kt must match).
 *
 * Every message on the AOA bulk pipes is one frame:
 *
 *   type u8 | len u16 LE | payload[len]          (len <= PROTO_MAX_PAYLOAD)
 *
 *   HELLO       0x01  both   "who I am" (UTF-8). Sent by each side when its
 *                            session starts; the peer answers HELLO_REPLY.
 *   (0x02, 0x03)             retired: TEXT / ACK, text messages from the
 *                            phone, dropped on 2026-10-25. Not to be reused;
 *                            an old phone app's TEXT is logged and ignored.
 *   HELLO_REPLY 0x04  both   "who I am", answer to HELLO (never answered)
 *
 * Video (a few fps of screen mirroring, one JPEG or PNG per picture):
 *
 *   VIDEO_CAPS  0x05  MMI    land_w u16, land_h u16, port_w u16, port_h u16,
 *                            max_bytes u32, formats u8 (bit n-1 = format n:
 *                            bit 0 JPEG, bit 1 PNG, bit 2 H.264 - an MMI
 *                            app from before 2026-10-30 sets JPEG only,
 *                            before 2026-11-04 no H.264). The boxes a
 *                            landscape / portrait picture has to fit in.
 *                            Since 2026-11-04 (22 bytes) followed by the
 *                            H.264 limits: h264_max_w u16, h264_max_h u16,
 *                            h264_fps u8, h264_max_frame u32 (the largest
 *                            frame_bytes H264_CONFIG may ask for).
 *                            Sent after HELLO and with HELLO_REPLY; a phone
 *                            streams only to an MMI that sent it.
 *   VIDEO_START 0x06  phone  id u16, w u16, h u16, format u8 (PROTO_FMT_*),
 *                            size u32 [, phone_ms u16]: a picture of size
 *                            bytes follows ... phone_ms (since 2026-10-30,
 *                            for the perf log only) is what the phone spent
 *                            on it: scaling + encoding. Longer payloads are
 *                            fine, the rest is ignored.
 *   VIDEO_DATA  0x07  phone  id u16 + the next bytes of it, in order
 *   VIDEO_ACK   0x08  MMI    id u16, status u8 (VIDEO_*), decode_ms u16:
 *                            the picture is on screen (or was dropped). The
 *                            phone keeps at most two pictures in flight.
 *   VIDEO_STOP  0x09  phone  (empty) streaming ended, back to the status
 *                            screen
 *   VIDEO_DETAILS 0x0A phone show u8: 1 = the status bar (state, video
 *                            stats) over a landscape picture, 0 = the
 *                            picture alone (the MMI's default). Sent on
 *                            the MMI's HELLO / HELLO_REPLY and whenever
 *                            the user flips it. Portrait always has its
 *                            status panel beside the picture.
 *
 * Hardware H.264 (since 2026-11-04; docs/phone-mirroring.md 7.10): the
 * unit's DM642 decodes, io-media plays, nothing is drawn by the CPU.
 *
 *   H264_CONFIG 0x0B  phone  fps u8, frame_bytes u32, sps_len u16, sps,
 *                            pps_len u16, pps. The encoder's SPS and PPS
 *                            (NAL units, header byte first, no start code;
 *                            MediaCodec's csd-0 / csd-1 without it). fps is
 *                            the rate the stream is timed at (the phone
 *                            should keep sending at it, repeating frames
 *                            when the screen is still: the MMI fills gaps
 *                            with skip frames, but late). frame_bytes: no
 *                            frame will be bigger (4-byte length per NAL
 *                            unit included); every frame is padded to it,
 *                            so it is also the bandwidth: frame_bytes x
 *                            fps. Picture at most h264_max_w x h264_max_h
 *                            (VIDEO_CAPS), Baseline, level 3.0 at most.
 *                            Sent before the first frame and whenever the
 *                            encoder is set up again (size, rotation). The
 *                            same config again changes nothing; a new one
 *                            restarts the stream (~1-2 s black).
 *   (VIDEO_START / VIDEO_DATA with format PROTO_FMT_H264: one encoded frame,
 *   Annex-B (start codes), as MediaCodec hands it out; w/h the stream's
 *   size. Acked at once on arrival - SHOWN: in the stream, DROPPED: waiting
 *   for an IDR, BAD: refused (too big, no config, ...). The first frame
 *   after a config must be an IDR.)
 *   H264_STATUS 0x0C  MMI    code u8 (PROTO_H264_*):
 *                            PLAYING  config accepted, the stream starts
 *                                     at the next IDR
 *                            SYNC     send an IDR now (start, or a frame
 *                                     was dropped and the ones after it
 *                                     can't be decoded)
 *                            REFUSED  config unusable (log says why): send
 *                                     JPEG / PNG pictures instead
 *                            CONFIG   send H264_CONFIG again (in-band SPS
 *                                     / PPS differ, or the stream ended)
 *
 * Frames may be split or merged arbitrarily across USB transfers; the
 * decoder reassembles them. A frame announcing more than PROTO_MAX_PAYLOAD
 * is skipped (its bytes are counted off) so the stream stays in sync.
 */
#ifndef AUDI_STREAM_PROTO_H
#define AUDI_STREAM_PROTO_H

#include <stdint.h>

#define PROTO_HELLO        0x01
#define PROTO_HELLO_REPLY  0x04
#define PROTO_VIDEO_CAPS   0x05
#define PROTO_VIDEO_START  0x06
#define PROTO_VIDEO_DATA   0x07
#define PROTO_VIDEO_ACK    0x08
#define PROTO_VIDEO_STOP   0x09
#define PROTO_VIDEO_DETAILS 0x0A
#define PROTO_H264_CONFIG  0x0B
#define PROTO_H264_STATUS  0x0C

#define PROTO_FMT_JPEG     1
#define PROTO_FMT_PNG      2
#define PROTO_FMT_H264     3

/* H264_STATUS code */
#define PROTO_H264_PLAYING 0
#define PROTO_H264_SYNC    1
#define PROTO_H264_REFUSED 2
#define PROTO_H264_CONFIG_WANTED 3

/* VIDEO_ACK status */
#define PROTO_VIDEO_SHOWN    0
#define PROTO_VIDEO_BAD      1   /* not decodable, or bigger than its box */
#define PROTO_VIDEO_DROPPED  2   /* superseded or cut short before shown */

#define PROTO_VIDEO_CAPS_LEN  22  /* 13 before 2026-11-04 */
#define PROTO_H264_CONFIG_MIN 11  /* fps, frame_bytes, two lengths, 1-byte SPS/PPS */
#define PROTO_VIDEO_START_LEN 11  /* the least; 13 with phone_ms */
#define PROTO_VIDEO_ACK_LEN   5
#define PROTO_VIDEO_DETAILS_LEN 1

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

/* H264_CONFIG payload; sps / pps point into p. 0, or -1 if malformed
 * (short, lengths past the end, an empty SPS / PPS, fps 0). */
struct proto_h264_config {
	unsigned       fps;
	uint32_t       frame_bytes;
	const uint8_t *sps, *pps;
	unsigned       sps_len, pps_len;
};
int proto_h264_config_parse(const uint8_t *p, unsigned n, struct proto_h264_config *out);

/* Encode one frame into out[cap]. Returns its size, or 0 if len is over
 * PROTO_MAX_PAYLOAD or the frame does not fit in cap. */
unsigned proto_encode(uint8_t *out, unsigned cap, uint8_t type,
                      const void *payload, unsigned len);

static inline const char *proto_fmt_name(unsigned format)
{
	return format == PROTO_FMT_PNG ? "PNG" : format == PROTO_FMT_H264 ? "H.264" : "JPEG";
}

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

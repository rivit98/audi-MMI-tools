/*
 * proto.c - audi_stream_receiver frame encoder / incremental decoder (see proto.h).
 */
#include <string.h>

#include "proto.h"

void proto_dec_init(struct proto_dec *d)
{
	d->have = 0;
	d->skip = 0;
	d->oversized = 0;
}

void proto_dec_feed(struct proto_dec *d, const uint8_t *data, unsigned n,
                    proto_frame_cb cb, void *arg)
{
	while (n > 0) {
		unsigned want, take, len;

		if (d->skip > 0) {                  /* dropping an oversized frame */
			take = n < d->skip ? n : d->skip;
			d->skip -= take;
			data += take;
			n -= take;
			continue;
		}
		if (d->have < PROTO_HDR) {
			want = PROTO_HDR - d->have;
		} else {
			len = proto_get_u16(d->buf + 1);
			want = PROTO_HDR + len - d->have;
		}
		take = n < want ? n : want;
		memcpy(d->buf + d->have, data, take);
		d->have += take;
		data += take;
		n -= take;
		if (d->have < PROTO_HDR)
			continue;

		len = proto_get_u16(d->buf + 1);
		if (len > PROTO_MAX_PAYLOAD) {
			d->oversized++;
			d->skip = len;
			d->have = 0;
			continue;
		}
		if (d->have == PROTO_HDR + len) {
			if (cb != NULL)
				cb(d->buf[0], d->buf + PROTO_HDR, len, arg);
			d->have = 0;
		}
	}
}

unsigned proto_encode(uint8_t *out, unsigned cap, uint8_t type,
                      const void *payload, unsigned len)
{
	if (len > PROTO_MAX_PAYLOAD || PROTO_HDR + len > cap)
		return 0;
	out[0] = type;
	out[1] = (uint8_t)(len & 0xff);
	out[2] = (uint8_t)(len >> 8);
	if (len > 0)
		memcpy(out + PROTO_HDR, payload, len);
	return PROTO_HDR + len;
}

int proto_h264_config_parse(const uint8_t *p, unsigned n, struct proto_h264_config *out)
{
	unsigned at;

	if (n < PROTO_H264_CONFIG_MIN)
		return -1;
	out->fps = p[0];
	out->frame_bytes = proto_get_u32(p + 1);
	out->sps_len = proto_get_u16(p + 5);
	at = 7;
	if (out->sps_len == 0 || at + out->sps_len + 2 > n)
		return -1;
	out->sps = p + at;
	at += out->sps_len;
	out->pps_len = proto_get_u16(p + at);
	at += 2;
	if (out->pps_len == 0 || at + out->pps_len > n)
		return -1;
	out->pps = p + at;
	return out->fps == 0 ? -1 : 0;
}

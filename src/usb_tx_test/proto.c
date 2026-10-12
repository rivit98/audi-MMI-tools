/*
 * proto.c - usb_tx_test frame encoder / incremental decoder (see proto.h).
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

/* test_proto.c - frame encoder/decoder (src/usb_tx_test/proto.c). */
#include "check.h"
#include "proto.h"

struct got {
	int n;
	uint8_t type[16];
	unsigned len[16];
	uint8_t payload[16][PROTO_MAX_PAYLOAD];
};

static void collect(uint8_t type, const uint8_t *payload, unsigned len, void *arg)
{
	struct got *g = arg;

	if (g->n >= 16)
		return;
	g->type[g->n] = type;
	g->len[g->n] = len;
	memcpy(g->payload[g->n], payload, len);
	g->n++;
}

static struct proto_dec dec;
static struct got got;
static uint8_t buf[3 * PROTO_MAX_FRAME];

static void reset(void)
{
	proto_dec_init(&dec);
	memset(&got, 0, sizeof(got));
}

static void test_encode(void)
{
	uint8_t big[PROTO_MAX_PAYLOAD + 1] = { 0 };
	unsigned n = proto_encode(buf, sizeof(buf), PROTO_TEXT, "\x05\x00hi", 4);

	CHECK_EQ(n, 7);
	CHECK_EQ(buf[0], PROTO_TEXT);
	CHECK_EQ(buf[1], 4);
	CHECK_EQ(buf[2], 0);
	CHECK(memcmp(buf + 3, "\x05\x00hi", 4) == 0);

	CHECK_EQ(proto_encode(buf, sizeof(buf), PROTO_ACK, NULL, 0), 3);
	CHECK_EQ(proto_encode(buf, 6, PROTO_TEXT, "\x05\x00hi", 4), 0);   /* no room */
	CHECK_EQ(proto_encode(buf, sizeof(buf), PROTO_TEXT, big, sizeof(big)), 0);
	CHECK_EQ(proto_encode(buf, sizeof(buf), PROTO_TEXT, big, PROTO_MAX_PAYLOAD),
	         PROTO_MAX_FRAME);
	/* length is little endian */
	CHECK_EQ(buf[1], PROTO_MAX_PAYLOAD & 0xff);
	CHECK_EQ(buf[2], PROTO_MAX_PAYLOAD >> 8);
}

static void test_whole_frames(void)
{
	unsigned n;

	reset();
	n = proto_encode(buf, sizeof(buf), PROTO_HELLO, "phone", 5);
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_ACK, "\x01\x02", 2);
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_HELLO_REPLY, NULL, 0);
	proto_dec_feed(&dec, buf, n, collect, &got);

	CHECK_EQ(got.n, 3);
	CHECK_EQ(got.type[0], PROTO_HELLO);
	CHECK_EQ(got.len[0], 5);
	CHECK(memcmp(got.payload[0], "phone", 5) == 0);
	CHECK_EQ(got.type[1], PROTO_ACK);
	CHECK_EQ(proto_get_u16(got.payload[1]), 0x0201);
	CHECK_EQ(got.type[2], PROTO_HELLO_REPLY);
	CHECK_EQ(got.len[2], 0);
}

static void test_byte_by_byte(void)
{
	unsigned n, i;

	reset();
	n = proto_encode(buf, sizeof(buf), PROTO_TEXT, "\x07\x00zażółć", 2 + 10);
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_TEXT, "\x08\x00x", 3);
	for (i = 0; i < n; i++)
		proto_dec_feed(&dec, buf + i, 1, collect, &got);

	CHECK_EQ(got.n, 2);
	CHECK_EQ(got.len[0], 12);
	CHECK_EQ(proto_get_u16(got.payload[0]), 7);
	CHECK(memcmp(got.payload[0] + 2, "zażółć", 10) == 0);
	CHECK_EQ(proto_get_u16(got.payload[1]), 8);
}

static void test_split_in_header(void)
{
	unsigned n;

	reset();
	n = proto_encode(buf, sizeof(buf), PROTO_HELLO, "abc", 3);
	proto_dec_feed(&dec, buf, 2, collect, &got);       /* type + half len */
	CHECK_EQ(got.n, 0);
	proto_dec_feed(&dec, buf + 2, n - 2, collect, &got);
	CHECK_EQ(got.n, 1);
	CHECK_EQ(got.len[0], 3);
}

static void test_oversized_skipped(void)
{
	unsigned n, i;
	unsigned big = PROTO_MAX_PAYLOAD + 10;

	reset();
	/* hand-made oversized frame, then a good one */
	buf[0] = PROTO_TEXT;
	buf[1] = big & 0xff;
	buf[2] = big >> 8;
	for (i = 0; i < big; i++)
		buf[3 + i] = 0xAA;
	n = 3 + big;
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_ACK, "\x09\x00", 2);

	/* in three uneven pieces, so the skip crosses feeds */
	proto_dec_feed(&dec, buf, 100, collect, &got);
	proto_dec_feed(&dec, buf + 100, 1000, collect, &got);
	proto_dec_feed(&dec, buf + 1100, n - 1100, collect, &got);

	CHECK_EQ(dec.oversized, 1);
	CHECK_EQ(got.n, 1);
	CHECK_EQ(got.type[0], PROTO_ACK);
	CHECK_EQ(proto_get_u16(got.payload[0]), 9);
}

static void test_max_payload_accepted(void)
{
	unsigned n;
	static uint8_t p[PROTO_MAX_PAYLOAD];

	reset();
	memset(p, 'x', sizeof(p));
	n = proto_encode(buf, sizeof(buf), PROTO_TEXT, p, sizeof(p));
	proto_dec_feed(&dec, buf, n, collect, &got);
	CHECK_EQ(got.n, 1);
	CHECK_EQ(got.len[0], PROTO_MAX_PAYLOAD);
	CHECK_EQ(dec.oversized, 0);
}

int main(void)
{
	test_encode();
	test_whole_frames();
	test_byte_by_byte();
	test_split_in_header();
	test_oversized_skipped();
	test_max_payload_accepted();
	CHECK_DONE();
}

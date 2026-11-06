/* test_proto.c - frame encoder/decoder (src/audi_stream_receiver/proto.c) and
 * video picture reassembly (src/audi_stream_receiver/vasm.c). */
#include <stdlib.h>

#include "check.h"
#include "proto.h"
#include "vasm.h"

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
	unsigned n = proto_encode(buf, sizeof(buf), PROTO_VIDEO_DATA, "\x05\x00hi", 4);

	CHECK_EQ(n, 7);
	CHECK_EQ(buf[0], PROTO_VIDEO_DATA);
	CHECK_EQ(buf[1], 4);
	CHECK_EQ(buf[2], 0);
	CHECK(memcmp(buf + 3, "\x05\x00hi", 4) == 0);

	CHECK_EQ(proto_encode(buf, sizeof(buf), PROTO_VIDEO_STOP, NULL, 0), 3);
	CHECK_EQ(proto_encode(buf, 6, PROTO_VIDEO_DATA, "\x05\x00hi", 4), 0);   /* no room */
	CHECK_EQ(proto_encode(buf, sizeof(buf), PROTO_VIDEO_DATA, big, sizeof(big)), 0);
	CHECK_EQ(proto_encode(buf, sizeof(buf), PROTO_VIDEO_DATA, big, PROTO_MAX_PAYLOAD),
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
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_VIDEO_ACK, "\x01\x02", 2);
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_HELLO_REPLY, NULL, 0);
	proto_dec_feed(&dec, buf, n, collect, &got);

	CHECK_EQ(got.n, 3);
	CHECK_EQ(got.type[0], PROTO_HELLO);
	CHECK_EQ(got.len[0], 5);
	CHECK(memcmp(got.payload[0], "phone", 5) == 0);
	CHECK_EQ(got.type[1], PROTO_VIDEO_ACK);
	CHECK_EQ(proto_get_u16(got.payload[1]), 0x0201);
	CHECK_EQ(got.type[2], PROTO_HELLO_REPLY);
	CHECK_EQ(got.len[2], 0);
}

static void test_byte_by_byte(void)
{
	unsigned n, i;

	reset();
	n = proto_encode(buf, sizeof(buf), PROTO_VIDEO_DATA, "\x07\x00zażółć", 2 + 10);
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_VIDEO_DATA, "\x08\x00x", 3);
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
	buf[0] = PROTO_VIDEO_DATA;
	buf[1] = big & 0xff;
	buf[2] = big >> 8;
	for (i = 0; i < big; i++)
		buf[3 + i] = 0xAA;
	n = 3 + big;
	n += proto_encode(buf + n, sizeof(buf) - n, PROTO_VIDEO_ACK, "\x09\x00", 2);

	/* in three uneven pieces, so the skip crosses feeds */
	proto_dec_feed(&dec, buf, 100, collect, &got);
	proto_dec_feed(&dec, buf + 100, 1000, collect, &got);
	proto_dec_feed(&dec, buf + 1100, n - 1100, collect, &got);

	CHECK_EQ(dec.oversized, 1);
	CHECK_EQ(got.n, 1);
	CHECK_EQ(got.type[0], PROTO_VIDEO_ACK);
	CHECK_EQ(proto_get_u16(got.payload[0]), 9);
}

static void test_max_payload_accepted(void)
{
	unsigned n;
	static uint8_t p[PROTO_MAX_PAYLOAD];

	reset();
	memset(p, 'x', sizeof(p));
	n = proto_encode(buf, sizeof(buf), PROTO_VIDEO_DATA, p, sizeof(p));
	proto_dec_feed(&dec, buf, n, collect, &got);
	CHECK_EQ(got.n, 1);
	CHECK_EQ(got.len[0], PROTO_MAX_PAYLOAD);
	CHECK_EQ(dec.oversized, 0);
}

/* --- vasm ------------------------------------------------------------- */

static unsigned start(uint8_t *p, unsigned id, unsigned w, unsigned h,
                      unsigned format, uint32_t size)
{
	proto_put_u16(p, id);
	proto_put_u16(p + 2, w);
	proto_put_u16(p + 4, h);
	p[6] = (uint8_t)format;
	proto_put_u32(p + 7, size);
	return PROTO_VIDEO_START_LEN;
}

static unsigned data(uint8_t *p, unsigned id, const char *bytes, unsigned n)
{
	proto_put_u16(p, id);
	memcpy(p + 2, bytes, n);
	return 2 + n;
}

static void test_u32_le(void)
{
	uint8_t b[4];

	proto_put_u32(b, 0x12345678u);
	CHECK_EQ(b[0], 0x78);
	CHECK_EQ(b[3], 0x12);
	CHECK_EQ(proto_get_u32(b), 0x12345678u);
}

static void test_vasm_whole_picture(void)
{
	struct vasm a;
	struct vpic pic;
	uint8_t p[64];
	int dropped, bad;
	unsigned n;

	vasm_init(&a, 100);
	n = start(p, 7, 640, 360, PROTO_FMT_JPEG, 10);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_OK);
	CHECK_EQ(dropped, -1);
	n = data(p, 7, "abcd", 4);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_OK);
	n = data(p, 8, "zz", 2);                     /* another id: ignored */
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_IGNORED);
	n = data(p, 7, "", 0);                       /* empty chunk is fine */
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_OK);
	n = data(p, 7, "efghij", 6);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_DONE);
	vasm_take(&a, &pic);
	CHECK_EQ(pic.id, 7);
	CHECK_EQ(pic.w, 640);
	CHECK_EQ(pic.h, 360);
	CHECK_EQ(pic.size, 10);
	CHECK(memcmp(pic.data, "abcdefghij", 10) == 0);
	free(pic.data);
	/* nothing in progress now */
	n = data(p, 7, "x", 1);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_IGNORED);
	vasm_free(&a);
}

static void test_vasm_refusals(void)
{
	struct vasm a;
	uint8_t p[64];
	int dropped, bad;
	unsigned n;

	vasm_init(&a, 100);
	n = start(p, 1, 640, 360, PROTO_FMT_JPEG, 101);   /* over max_bytes */
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_BAD);
	CHECK_EQ(bad, 1);
	n = start(p, 2, 640, 360, PROTO_FMT_JPEG, 0);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_BAD);
	n = start(p, 3, 640, 360, 9, 10);                 /* unknown format */
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_BAD);
	n = start(p, 3, 640, 360, 0, 10);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_BAD);
	n = start(p, 4, 0, 360, PROTO_FMT_JPEG, 10);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_BAD);
	CHECK_EQ(vasm_start(&a, p, 5, &dropped, &bad), VASM_BAD);   /* short */
	CHECK_EQ(bad, 4);

	/* overflow: more data than announced drops the picture */
	n = start(p, 5, 10, 10, PROTO_FMT_JPEG, 3);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_OK);
	n = data(p, 5, "abcd", 4);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_BAD);
	CHECK_EQ(bad, 5);
	CHECK_EQ(a.active, 0);
	vasm_free(&a);
}

/* PNG, and phone_ms: absent in an 11-byte START (old phone app), read from
 * a 13-byte one, extra bytes after it ignored. */
static void test_vasm_png_phone_ms(void)
{
	struct vasm a;
	struct vpic pic;
	uint8_t p[64];
	int dropped, bad;
	unsigned n;

	vasm_init(&a, 100);
	n = start(p, 20, 800, 368, PROTO_FMT_PNG, 2);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_OK);
	n = data(p, 20, "ok", 2);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_DONE);
	vasm_take(&a, &pic);
	CHECK_EQ(pic.format, PROTO_FMT_PNG);
	CHECK_EQ(pic.phone_ms, 0);
	free(pic.data);

	n = start(p, 21, 800, 368, PROTO_FMT_JPEG, 1);
	proto_put_u16(p + n, 345);
	p[n + 2] = 0x77;
	CHECK_EQ(vasm_start(&a, p, n + 3, &dropped, &bad), VASM_OK);
	n = data(p, 21, "j", 1);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_DONE);
	vasm_take(&a, &pic);
	CHECK_EQ(pic.format, PROTO_FMT_JPEG);
	CHECK_EQ(pic.phone_ms, 345);
	free(pic.data);
	vasm_free(&a);
}

static void test_vasm_cut_short(void)
{
	struct vasm a;
	struct vpic pic;
	uint8_t p[64];
	int dropped, bad;
	unsigned n;

	vasm_init(&a, 100);
	n = start(p, 10, 10, 20, PROTO_FMT_JPEG, 4);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_OK);
	n = data(p, 10, "ab", 2);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_OK);
	n = start(p, 11, 10, 20, PROTO_FMT_JPEG, 2);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_OK);
	CHECK_EQ(dropped, 10);
	n = data(p, 10, "cd", 2);                    /* the old one's rest */
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_IGNORED);
	n = data(p, 11, "XY", 2);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_DONE);
	vasm_take(&a, &pic);
	CHECK_EQ(pic.id, 11);
	CHECK(memcmp(pic.data, "XY", 2) == 0);
	free(pic.data);
	vasm_free(&a);
}

/* H.264 frames go through the same VIDEO_START / DATA; H264_CONFIG parses. */
static void test_h264(void)
{
	struct vasm a;
	struct vpic pic;
	struct proto_h264_config c;
	uint8_t p[64];
	int dropped, bad;
	unsigned n;
	const uint8_t cfg[] = {25, 0x00, 0x70, 0x00, 0x00,     /* 25 fps, 28672 bytes */
	                       3, 0, 0x67, 0x42, 0xc0,          /* SPS, 3 bytes */
	                       2, 0, 0x68, 0xce};               /* PPS, 2 bytes */

	vasm_init(&a, 100);
	n = start(p, 30, 720, 432, PROTO_FMT_H264, 3);
	CHECK_EQ(vasm_start(&a, p, n, &dropped, &bad), VASM_OK);
	n = data(p, 30, "abc", 3);
	CHECK_EQ(vasm_data(&a, p, n, &bad), VASM_DONE);
	vasm_take(&a, &pic);
	CHECK_EQ(pic.format, PROTO_FMT_H264);
	free(pic.data);
	vasm_free(&a);
	CHECK_STR(proto_fmt_name(PROTO_FMT_H264), "H.264");

	CHECK_EQ(proto_h264_config_parse(cfg, sizeof(cfg), &c), 0);
	CHECK_EQ(c.fps, 25);
	CHECK_EQ(c.frame_bytes, 28672);
	CHECK_EQ(c.sps_len, 3);
	CHECK_EQ(c.sps[0], 0x67);
	CHECK_EQ(c.pps_len, 2);
	CHECK_EQ(c.pps[1], 0xce);
	CHECK_EQ(proto_h264_config_parse(cfg, sizeof(cfg) - 1, &c), -1);   /* PPS cut */
	CHECK_EQ(proto_h264_config_parse(cfg, 10, &c), -1);                /* short */
	memcpy(p, cfg, sizeof(cfg));
	p[0] = 0;
	CHECK_EQ(proto_h264_config_parse(p, sizeof(cfg), &c), -1);         /* 0 fps */
	p[0] = 25;
	p[5] = 0;
	CHECK_EQ(proto_h264_config_parse(p, sizeof(cfg), &c), -1);         /* empty SPS */
}

int main(void)
{
	test_u32_le();
	test_vasm_whole_picture();
	test_vasm_refusals();
	test_vasm_cut_short();
	test_vasm_png_phone_ms();
	test_encode();
	test_whole_frames();
	test_byte_by_byte();
	test_split_in_header();
	test_oversized_skipped();
	test_max_payload_accepted();
	test_h264();
	CHECK_DONE();
}

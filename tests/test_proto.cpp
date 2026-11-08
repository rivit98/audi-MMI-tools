// test_proto.cpp - frame encoder/decoder (src/audi_stream_receiver/proto.h),
// video picture reassembly (vasm.h) and the VIDEO_ACKS batch (ack_batch.h).
#include <vector>

#include "check.h"
#include "ack_batch.h"
#include "proto.h"
#include "vasm.h"

using namespace proto;

namespace {

struct Frame {
	uint8_t type;
	std::vector<uint8_t> payload;
};

std::vector<Frame> got;
Decoder *dec;
uint8_t buf[3 * kMaxFrame];

void feed(const uint8_t *p, unsigned n)
{
	dec->feed(p, n, [](uint8_t type, const uint8_t *payload, unsigned len) {
		got.push_back(Frame{type, std::vector<uint8_t>(payload, payload + len)});
	});
}

void reset()
{
	delete dec;
	dec = new Decoder();
	got.clear();
}

} // namespace

static void test_encode(void)
{
	uint8_t big[kMaxPayload + 1] = { 0 };
	unsigned n = encode(buf, sizeof(buf), kVideoData, "\x05\x00hi", 4);

	CHECK_EQ(n, 7);
	CHECK_EQ(buf[0], kVideoData);
	CHECK_EQ(buf[1], 4);
	CHECK_EQ(buf[2], 0);
	CHECK(memcmp(buf + 3, "\x05\x00hi", 4) == 0);

	CHECK_EQ(encode(buf, sizeof(buf), kVideoStop, nullptr, 0), 3);
	CHECK_EQ(encode(buf, 6, kVideoData, "\x05\x00hi", 4), 0);   /* no room */
	CHECK_EQ(encode(buf, sizeof(buf), kVideoData, big, sizeof(big)), 0);
	CHECK_EQ(encode(buf, sizeof(buf), kVideoData, big, kMaxPayload),
	         kMaxFrame);
	/* length is little endian */
	CHECK_EQ(buf[1], kMaxPayload & 0xff);
	CHECK_EQ(buf[2], kMaxPayload >> 8);
}

static void test_whole_frames(void)
{
	unsigned n;

	reset();
	n = encode(buf, sizeof(buf), kHello, "phone", 5);
	n += encode(buf + n, sizeof(buf) - n, kVideoAcks, "\x01\x02", 2);
	n += encode(buf + n, sizeof(buf) - n, kHelloReply, nullptr, 0);
	feed(buf, n);

	CHECK_EQ(got.size(), 3);
	CHECK_EQ(got[0].type, kHello);
	CHECK_EQ(got[0].payload.size(), 5);
	CHECK(memcmp(got[0].payload.data(), "phone", 5) == 0);
	CHECK_EQ(got[1].type, kVideoAcks);
	CHECK_EQ(get_u16(got[1].payload.data()), 0x0201);
	CHECK_EQ(got[2].type, kHelloReply);
	CHECK_EQ(got[2].payload.size(), 0);
}

static void test_byte_by_byte(void)
{
	unsigned n, i;

	reset();
	n = encode(buf, sizeof(buf), kVideoData, "\x07\x00zażółć", 2 + 10);
	n += encode(buf + n, sizeof(buf) - n, kVideoData, "\x08\x00x", 3);
	for (i = 0; i < n; i++)
		feed(buf + i, 1);

	CHECK_EQ(got.size(), 2);
	CHECK_EQ(got[0].payload.size(), 12);
	CHECK_EQ(get_u16(got[0].payload.data()), 7);
	CHECK(memcmp(got[0].payload.data() + 2, "zażółć", 10) == 0);
	CHECK_EQ(get_u16(got[1].payload.data()), 8);
}

static void test_split_in_header(void)
{
	unsigned n;

	reset();
	n = encode(buf, sizeof(buf), kHello, "abc", 3);
	feed(buf, 2);       /* type + half len */
	CHECK_EQ(got.size(), 0);
	feed(buf + 2, n - 2);
	CHECK_EQ(got.size(), 1);
	CHECK_EQ(got[0].payload.size(), 3);
}

static void test_oversized_skipped(void)
{
	unsigned n, i;
	unsigned big = kMaxPayload + 10;

	reset();
	/* hand-made oversized frame, then a good one */
	buf[0] = kVideoData;
	buf[1] = big & 0xff;
	buf[2] = big >> 8;
	for (i = 0; i < big; i++)
		buf[3 + i] = 0xAA;
	n = 3 + big;
	n += encode(buf + n, sizeof(buf) - n, kVideoAcks, "\x09\x00", 2);

	/* in three uneven pieces, so the skip crosses feeds */
	feed(buf, 100);
	feed(buf + 100, 1000);
	feed(buf + 1100, n - 1100);

	CHECK_EQ(dec->oversized(), 1);
	CHECK_EQ(got.size(), 1);
	CHECK_EQ(got[0].type, kVideoAcks);
	CHECK_EQ(get_u16(got[0].payload.data()), 9);
}

static void test_max_payload_accepted(void)
{
	unsigned n;
	static uint8_t p[kMaxPayload];

	reset();
	memset(p, 'x', sizeof(p));
	n = encode(buf, sizeof(buf), kVideoData, p, sizeof(p));
	feed(buf, n);
	CHECK_EQ(got.size(), 1);
	CHECK_EQ(got[0].payload.size(), kMaxPayload);
	CHECK_EQ(dec->oversized(), 0);
}

/* --- vasm ------------------------------------------------------------- */

static unsigned start(uint8_t *p, unsigned id, unsigned w, unsigned h,
                      unsigned format, uint32_t size)
{
	put_u16(p, id);
	put_u16(p + 2, w);
	put_u16(p + 4, h);
	p[6] = (uint8_t)format;
	put_u32(p + 7, size);
	return kVideoStartLen;
}

static unsigned data(uint8_t *p, unsigned id, const char *bytes, unsigned n)
{
	put_u16(p, id);
	memcpy(p + 2, bytes, n);
	return 2 + n;
}

static void test_u32_le(void)
{
	uint8_t b[4];

	put_u32(b, 0x12345678u);
	CHECK_EQ(b[0], 0x78);
	CHECK_EQ(b[3], 0x12);
	CHECK_EQ(get_u32(b), 0x12345678u);
}

static void test_vasm_whole_picture(void)
{
	Assembler a(100);
	uint8_t p[64];
	unsigned n;

	n = start(p, 7, 640, 360, kFmtJpeg, 10);
	Assembler::Result r = a.start(p, n);
	CHECK(r.rx == Assembler::Rx::Ok);
	CHECK_EQ(r.cut_short, -1);
	n = data(p, 7, "abcd", 4);
	CHECK(a.data(p, n).rx == Assembler::Rx::Ok);
	n = data(p, 8, "zz", 2);                     // another id: ignored
	CHECK(a.data(p, n).rx == Assembler::Rx::Ignored);
	n = data(p, 7, "", 0);                       // empty chunk is fine
	CHECK(a.data(p, n).rx == Assembler::Rx::Ok);
	n = data(p, 7, "efghij", 6);
	CHECK(a.data(p, n).rx == Assembler::Rx::Done);
	Picture pic = a.take();
	CHECK_EQ(pic.id, 7);
	CHECK_EQ(pic.w, 640);
	CHECK_EQ(pic.h, 360);
	CHECK_EQ(pic.size, 10);
	CHECK(memcmp(pic.data.get(), "abcdefghij", 10) == 0);
	// nothing in progress now
	n = data(p, 7, "x", 1);
	CHECK(a.data(p, n).rx == Assembler::Rx::Ignored);
}

static void test_vasm_refusals(void)
{
	Assembler a(100);
	uint8_t p[64];
	unsigned n;

	n = start(p, 1, 640, 360, kFmtJpeg, 101);    // over max_bytes
	Assembler::Result r = a.start(p, n);
	CHECK(r.rx == Assembler::Rx::Bad);
	CHECK_EQ(r.id, 1);
	n = start(p, 2, 640, 360, kFmtJpeg, 0);
	CHECK(a.start(p, n).rx == Assembler::Rx::Bad);
	n = start(p, 3, 640, 360, 9, 10);            // unknown format
	CHECK(a.start(p, n).rx == Assembler::Rx::Bad);
	n = start(p, 3, 640, 360, 2, 10);            // PNG, retired
	CHECK(a.start(p, n).rx == Assembler::Rx::Bad);
	n = start(p, 3, 640, 360, 0, 10);
	CHECK(a.start(p, n).rx == Assembler::Rx::Bad);
	n = start(p, 4, 0, 360, kFmtJpeg, 10);
	CHECK(a.start(p, n).rx == Assembler::Rx::Bad);
	r = a.start(p, 5);                           // short
	CHECK(r.rx == Assembler::Rx::Bad);
	CHECK_EQ(r.id, 4);

	// overflow: more data than announced drops the picture
	n = start(p, 5, 10, 10, kFmtJpeg, 3);
	CHECK(a.start(p, n).rx == Assembler::Rx::Ok);
	n = data(p, 5, "abcd", 4);
	r = a.data(p, n);
	CHECK(r.rx == Assembler::Rx::Bad);
	CHECK_EQ(r.id, 5);
	CHECK(!a.active());
}

// phone_ms: absent in an 11-byte START (old phone app), read from a 13-byte
// one, extra bytes after it ignored.
static void test_vasm_phone_ms(void)
{
	Assembler a(100);
	uint8_t p[64];
	unsigned n;

	n = start(p, 20, 800, 368, kFmtJpeg, 2);
	CHECK(a.start(p, n).rx == Assembler::Rx::Ok);
	n = data(p, 20, "ok", 2);
	CHECK(a.data(p, n).rx == Assembler::Rx::Done);
	Picture pic = a.take();
	CHECK_EQ(pic.format, kFmtJpeg);
	CHECK_EQ(pic.phone_ms, 0);

	n = start(p, 21, 800, 368, kFmtJpeg, 1);
	put_u16(p + n, 345);
	p[n + 2] = 0x77;
	CHECK(a.start(p, n + 3).rx == Assembler::Rx::Ok);
	n = data(p, 21, "j", 1);
	CHECK(a.data(p, n).rx == Assembler::Rx::Done);
	pic = a.take();
	CHECK_EQ(pic.format, kFmtJpeg);
	CHECK_EQ(pic.phone_ms, 345);
}

static void test_vasm_cut_short(void)
{
	Assembler a(100);
	uint8_t p[64];
	unsigned n;

	n = start(p, 10, 10, 20, kFmtJpeg, 4);
	CHECK(a.start(p, n).rx == Assembler::Rx::Ok);
	n = data(p, 10, "ab", 2);
	CHECK(a.data(p, n).rx == Assembler::Rx::Ok);
	n = start(p, 11, 10, 20, kFmtJpeg, 2);
	Assembler::Result r = a.start(p, n);
	CHECK(r.rx == Assembler::Rx::Ok);
	CHECK_EQ(r.cut_short, 10);
	n = data(p, 10, "cd", 2);                    // the old one's rest
	CHECK(a.data(p, n).rx == Assembler::Rx::Ignored);
	n = data(p, 11, "XY", 2);
	CHECK(a.data(p, n).rx == Assembler::Rx::Done);
	Picture pic = a.take();
	CHECK_EQ(pic.id, 11);
	CHECK(memcmp(pic.data.get(), "XY", 2) == 0);
}

// H.264 frames go through the same VIDEO_START / DATA; H264_CONFIG parses.
static void test_h264(void)
{
	Assembler a(100);
	uint8_t p[64];
	unsigned n;
	const uint8_t cfg[] = {25, 0x00, 0x70, 0x00, 0x00,     // 25 fps, 28672 bytes
	                       3, 0, 0x67, 0x42, 0xc0,          // SPS, 3 bytes
	                       2, 0, 0x68, 0xce};               // PPS, 2 bytes

	n = start(p, 30, 720, 432, kFmtH264, 3);
	CHECK(a.start(p, n).rx == Assembler::Rx::Ok);
	n = data(p, 30, "abc", 3);
	CHECK(a.data(p, n).rx == Assembler::Rx::Done);
	Picture pic = a.take();
	CHECK_EQ(pic.format, kFmtH264);
	CHECK_STR(fmt_name(kFmtH264), "H.264");

	auto c = parse_h264_config(cfg, sizeof(cfg));
	CHECK(c.has_value());
	CHECK_EQ(c->fps, 25);
	CHECK_EQ(c->frame_bytes, 28672);
	CHECK_EQ(c->sps_len, 3);
	CHECK_EQ(c->sps[0], 0x67);
	CHECK_EQ(c->pps_len, 2);
	CHECK_EQ(c->pps[1], 0xce);
	CHECK_EQ(c->vcap_mode, kVcapKeep);                 // an older phone app
	memcpy(p, cfg, sizeof(cfg));
	p[sizeof(cfg)] = kVcapOddOnly;
	c = parse_h264_config(p, sizeof(cfg) + 1);
	CHECK(c.has_value());
	CHECK_EQ(c->vcap_mode, kVcapOddOnly);
	p[sizeof(cfg)] = kVcapOddOnly + 1;
	c = parse_h264_config(p, sizeof(cfg) + 1);
	CHECK(c.has_value());
	CHECK_EQ(c->vcap_mode, kVcapKeep);                 // unknown
	CHECK(!parse_h264_config(cfg, sizeof(cfg) - 1));   // PPS cut
	CHECK(!parse_h264_config(cfg, 10));                // short
	memcpy(p, cfg, sizeof(cfg));
	p[0] = 0;
	CHECK(!parse_h264_config(p, sizeof(cfg)));         // 0 fps
	p[0] = 25;
	p[5] = 0;
	CHECK(!parse_h264_config(p, sizeof(cfg)));         // empty SPS
}

// --- VIDEO_ACKS --------------------------------------------------------

static void test_ack_batch(void)
{
	AckBatch b;

	CHECK_EQ(b.take().total(), 0);
	CHECK(!b.full());
	for (unsigned i = 0; i + 2 < AckBatch::kEveryFrames; i++)
		b.add(PicStatus::Shown);
	b.add(PicStatus::Dropped);
	CHECK(!b.full());
	b.add(PicStatus::Bad);
	CHECK(b.full());

	AckBatch::Counts c = b.take();
	CHECK_EQ(c.shown, AckBatch::kEveryFrames - 2);
	CHECK_EQ(c.bad, 1);
	CHECK_EQ(c.dropped, 1);
	CHECK_EQ(c.total(), AckBatch::kEveryFrames);
	CHECK(!b.full());
	CHECK_EQ(b.take().total(), 0);               // taken means gone

	b.put_back(c);                               // could not be sent
	b.add(PicStatus::Shown);
	c = b.take();
	CHECK_EQ(c.shown, AckBatch::kEveryFrames - 1);
	CHECK_EQ(c.bad, 1);

	b.put_back(c);
	b.clear();
	CHECK_EQ(b.take().total(), 0);
}

// proto.h, and Proto.kt's videoAcksOrNull: shown u16, bad u16, dropped u16
static void test_ack_layout(void)
{
	AckBatch::Counts c;
	uint8_t p[kVideoAcksLen];

	c.shown = 0x0102;
	c.bad = 3;
	c.dropped = 0x0405;
	c.encode(p);
	const uint8_t want[kVideoAcksLen] = {0x02, 0x01, 3, 0, 0x05, 0x04};
	CHECK(memcmp(p, want, sizeof(p)) == 0);
	CHECK_EQ(kVideoAcks, 0x0D);
}

// A count over u16 goes out in more than one frame, whole.
static void test_ack_big_count(void)
{
	AckBatch b;
	AckBatch::Counts many;

	many.dropped = AckBatch::kMaxCount + 7;
	b.put_back(many);
	AckBatch::Counts c = b.take();
	CHECK_EQ(c.dropped, AckBatch::kMaxCount);
	c = b.take();
	CHECK_EQ(c.dropped, 7);
	CHECK_EQ(b.take().total(), 0);
}

int main(void)
{
	test_u32_le();
	test_vasm_whole_picture();
	test_vasm_refusals();
	test_vasm_cut_short();
	test_vasm_phone_ms();
	test_encode();
	test_whole_frames();
	test_byte_by_byte();
	test_split_in_header();
	test_oversized_skipped();
	test_max_payload_accepted();
	test_h264();
	test_ack_batch();
	test_ack_layout();
	test_ack_big_count();
	delete dec;
	CHECK_DONE();
}

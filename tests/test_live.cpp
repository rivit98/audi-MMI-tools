// test_live.cpp - the pure part of the live H.264 stream (src/common
// h264, csmp4, live_stream; hello9's schedule) on the build host.
//
// With build/test_live.d/in.h264 and ref.mp4 there (tests/check_live.py
// prepare, run by make test in the mmi-media image), it also checks the
// header against media/csmp4.py's byte for byte, and writes a stream with
// a filled gap for check_live.py to decode with ffmpeg.
#include "check.h"
#include "csmp4.h"
#include "h264.h"
#include "live_stream.h"
#include "schedule.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// "" from a call returning std::string (CHECK_STR on its .c_str() would
// read a destroyed temporary)
#define CHECK_OK(expr) do { \
	std::string ok_ = (expr); \
	CHECK_STR(ok_.c_str(), ""); \
} while (0)

namespace {

// Big-endian box writer, the layout media/csmp4.py writes.
struct Buf {
	std::vector<uint8_t> b;
	void u8(uint32_t v) { b.push_back((uint8_t)v); }
	void u16(uint32_t v) { u8(v >> 8); u8(v); }
	void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
	void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
	void tag(const char *t) { for (int i = 0; i < 4; i++) u8((uint8_t)t[i]); }
	void zeros(int n) { b.insert(b.end(), (size_t)n, 0); }
	size_t open(const char *t)
	{
		size_t at = b.size();
		u32(0);
		tag(t);
		return at;
	}
	void close(size_t at)
	{
		uint32_t n = (uint32_t)(b.size() - at);
		b[at] = (uint8_t)(n >> 24);
		b[at + 1] = (uint8_t)(n >> 16);
		b[at + 2] = (uint8_t)(n >> 8);
		b[at + 3] = (uint8_t)n;
	}
};

struct Spec {
	uint32_t timescale = 12800, delta = 512;
	uint32_t size = 4096, declared = 100;
	uint32_t stts_entries = 1, stsc_per_chunk = 0;   // 0: = declared
	bool co64 = false;
	uint32_t present = 3;                            // frames written after the header
	int mdat = 0;          // mdat header: 0 exact 32-bit size, 1 size 0, 2 64-bit, 3 wrong size
};

std::vector<uint8_t> make(const Spec &s, uint64_t *data_off)
{
	Buf f;
	size_t a = f.open("ftyp");
	f.tag("isom");
	f.u32(0x200);
	f.tag("isom");
	f.close(a);

	size_t moov = f.open("moov");
	size_t mvhd = f.open("mvhd");
	f.zeros(100);
	f.close(mvhd);
	size_t trak = f.open("trak");
	size_t tkhd = f.open("tkhd");
	f.zeros(84);
	f.close(tkhd);
	size_t mdia = f.open("mdia");
	size_t mdhd = f.open("mdhd");
	f.u32(0);                         // version 0, flags
	f.u32(0);
	f.u32(0);
	f.u32(s.timescale);
	f.u32(s.declared * s.delta);
	f.u32(0x55c40000);
	f.close(mdhd);
	size_t minf = f.open("minf");
	size_t stbl = f.open("stbl");
	size_t stsd = f.open("stsd");
	f.u32(0);
	f.u32(1);
	size_t avc1 = f.open("avc1");
	f.zeros(78);
	f.close(avc1);
	f.close(stsd);
	size_t stts = f.open("stts");
	f.u32(0);
	f.u32(s.stts_entries);
	for (uint32_t i = 0; i < s.stts_entries; i++) {
		f.u32(s.declared / s.stts_entries);
		f.u32(s.delta);
	}
	f.close(stts);
	size_t stsc = f.open("stsc");
	f.u32(0);
	f.u32(1);
	f.u32(1);
	f.u32(s.stsc_per_chunk ? s.stsc_per_chunk : s.declared);
	f.u32(1);
	f.close(stsc);
	size_t stsz = f.open("stsz");
	f.u32(0);
	f.u32(s.size);
	f.u32(s.declared);
	f.close(stsz);
	size_t co = f.open(s.co64 ? "co64" : "stco");
	f.u32(0);
	f.u32(1);
	size_t stco_value = f.b.size();
	if (s.co64)
		f.u64(0);
	else
		f.u32(0);
	f.close(co);
	f.close(stbl);
	f.close(minf);
	f.close(mdia);
	f.close(trak);
	f.close(moov);

	const uint64_t payload = (uint64_t)s.declared * s.size;
	if (s.mdat == 2) {
		f.u32(1);                 // 64-bit size, as an older csmp4.py wrote
		f.tag("mdat");
		f.u64(16 + payload);
	} else {
		f.u32(s.mdat == 1 ? 0 : (uint32_t)(8 + payload + (s.mdat == 3 ? s.size : 0)));
		f.tag("mdat");
	}
	*data_off = f.b.size();
	if (!s.co64) {
		f.b[stco_value] = (uint8_t)(*data_off >> 24);
		f.b[stco_value + 1] = (uint8_t)(*data_off >> 16);
		f.b[stco_value + 2] = (uint8_t)(*data_off >> 8);
		f.b[stco_value + 3] = (uint8_t)*data_off;
	}
	for (uint32_t k = 0; k < s.present; k++)
		f.b.insert(f.b.end(), s.size, (uint8_t)('A' + k));
	return f.b;
}

void test_parse()
{
	Spec s;
	uint64_t data_off = 0;
	std::vector<uint8_t> file = make(s, &data_off);
	csmp4::Info m;
	std::string err = csmp4::parse(file.data(), file.size(), file.size(), m);

	CHECK_STR(err.c_str(), "");
	CHECK_EQ(m.data_off, data_off);
	CHECK_EQ(m.sample_size, 4096);
	CHECK_EQ(m.declared, 100);
	CHECK_EQ(m.present, 3);
	CHECK_EQ(m.timescale, 12800);
	CHECK_EQ(m.delta, 512);
	CHECK_EQ(m.total(), data_off + 100 * 4096);
	CHECK(m.fps() > 24.99 && m.fps() < 25.01);

	// a short read of the file: only the header
	csmp4::Info h;
	err = csmp4::parse(file.data(), (size_t)data_off, file.size(), h);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(h.present, 3);

	// a partial last frame does not count
	err = csmp4::parse(file.data(), file.size(), file.size() - 1, h);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(h.present, 2);

	// mdat size 0 (to the end of the file): an over-declared stream
	s.mdat = 1;
	file = make(s, &data_off);
	err = csmp4::parse(file.data(), file.size(), file.size(), m);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(m.data_off, data_off);
	CHECK_EQ(m.present, 3);
}

void test_parse_rejects()
{
	uint64_t off;
	csmp4::Info m;
	std::vector<uint8_t> f;
	Spec s;

	s.size = 0;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // per-sample sizes

	s = Spec();
	s.stts_entries = 2;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // two durations

	s = Spec();
	s.stsc_per_chunk = 10;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // several chunks

	s = Spec();
	s.co64 = true;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // co64

	s = Spec();
	s.present = 0;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // no frames

	s = Spec();
	s.mdat = 2;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // 64-bit mdat size

	s = Spec();
	s.mdat = 3;
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // mdat past the frames

	s = Spec();
	s.size = 65536;
	s.declared = 70000;                                                // 4.6 GB
	f = make(s, &off);
	CHECK(!csmp4::parse(f.data(), (size_t)off, f.size(), m).empty());  // over 4 GB

	f = make(Spec(), &off);
	CHECK(!csmp4::parse(f.data(), (size_t)off - 8, f.size(), m).empty()); // no mdat header
	CHECK(!csmp4::parse(f.data(), 40, f.size(), m).empty());           // truncated moov
	f[4] = 'x';
	CHECK(!csmp4::parse(f.data(), f.size(), f.size(), m).empty());     // not ftyp
}

void test_schedule()
{
	plan::Schedule s(25.0, {});

	CHECK_EQ(s.release_ms(0), 0);
	CHECK_EQ(s.release_ms(1), 40);
	CHECK_EQ(s.release_ms(25), 1000);
	CHECK_EQ(s.released(0), 1);
	CHECK_EQ(s.released(39), 1);
	CHECK_EQ(s.released(40), 2);
	CHECK_EQ(s.released(1000), 26);

	plan::Schedule st(25.0, {{10, 2000}, {20, 500}});
	CHECK_EQ(st.release_ms(9), 360);
	CHECK_EQ(st.release_ms(10), 2400);      // 400 + the 2 s stall
	CHECK_EQ(st.release_ms(19), 2760);
	CHECK_EQ(st.release_ms(20), 3300);      // 800 + 2000 + 500
	CHECK_EQ(st.released(2399), 10);
	CHECK_EQ(st.released(2400), 11);
	CHECK_EQ(st.released(3299), 20);
	CHECK_EQ(st.released(3300), 21);
	CHECK_EQ(st.stall_ms_before(9), 0);
	CHECK_EQ(st.stall_ms_before(10), 2000);
	CHECK_EQ(st.stall_ms_before(25), 2500);
}

// ---- H.264 helpers ----

// x264 Baseline 3.0, 720x336 (media/mkclips.sh CS264), and its PPS
const std::vector<uint8_t> kSps = {0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0xd0, 0xae,
                                   0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00,
                                   0x00, 0x03, 0x00, 0xc8, 0x3c, 0x58, 0xba, 0x80};
const std::vector<uint8_t> kPps = {0x68, 0xce, 0x3c, 0x80};

struct BitW {
	std::vector<uint8_t> out;
	uint8_t cur = 0;
	unsigned n = 0;
	void bit(unsigned b)
	{
		cur = (uint8_t)(cur << 1 | (b & 1));
		if (++n == 8) {
			out.push_back(cur);
			cur = 0;
			n = 0;
		}
	}
	void u(unsigned bits, unsigned v)
	{
		while (bits-- > 0)
			bit(v >> bits);
	}
	void ue(unsigned v)
	{
		unsigned x = v + 1, len = 0;
		for (unsigned t = x; t > 1; t >>= 1)
			len++;
		u(len, 0);
		u(len + 1, x);
	}
	void trailing()
	{
		bit(1);
		while (n != 0)
			bit(0);
	}
};

// A Baseline SPS with POC type 0 (x264 Baseline uses type 2): 176x144.
std::vector<uint8_t> sps_poc0()
{
	BitW w;
	w.u(8, 66);
	w.u(8, 0xc0);
	w.u(8, 30);
	w.ue(0);          // sps id
	w.ue(0);          // log2_max_frame_num - 4
	w.ue(0);          // poc type 0
	w.ue(2);          // log2_max_poc_lsb - 4
	w.ue(1);          // max refs
	w.u(1, 0);
	w.ue(10);         // 11 MBs wide
	w.ue(8);          // 9 MBs high
	w.u(1, 1);        // frame_mbs_only
	w.u(1, 1);
	w.u(1, 0);        // no cropping
	w.u(1, 0);        // no VUI
	w.trailing();
	std::vector<uint8_t> nal{0x67};
	h264::escape(w.out, nal);
	return nal;
}

// A slice NAL with a valid header start and `body` bytes of 0xaa after it.
std::vector<uint8_t> slice(const h264::Sps &sps, bool idr, unsigned ref_idc, unsigned first_mb,
                           unsigned frame_num, unsigned poc_lsb, size_t body)
{
	BitW w;
	w.ue(first_mb);
	w.ue(idr ? 7 : 5);
	w.ue(0);          // pps id
	w.u(sps.log2_max_frame_num, frame_num);
	if (idr)
		w.ue(0);
	if (sps.poc_type == 0)
		w.u(sps.log2_max_poc_lsb, poc_lsb);
	w.trailing();
	std::vector<uint8_t> nal{(uint8_t)(ref_idc << 5 | (idr ? 5 : 1))};
	h264::escape(w.out, nal);
	nal.insert(nal.end(), body, 0xaa);
	return nal;
}

void annexb(std::vector<uint8_t> &out, const std::vector<uint8_t> &nal)
{
	static const uint8_t sc[4] = {0, 0, 0, 1};
	out.insert(out.end(), sc, sc + 4);
	out.insert(out.end(), nal.begin(), nal.end());
}

std::vector<uint8_t> idr_au(const h264::Sps &sps, const std::vector<uint8_t> &sp,
                            const std::vector<uint8_t> &pp, size_t body = 300)
{
	std::vector<uint8_t> au;
	annexb(au, {0x09, 0xf0});        // AUD: dropped
	annexb(au, sp);
	annexb(au, pp);
	annexb(au, slice(sps, true, 3, 0, 0, 0, body));
	return au;
}

std::vector<uint8_t> p_au(const h264::Sps &sps, unsigned frame_num, size_t body = 200,
                          unsigned ref_idc = 2)
{
	std::vector<uint8_t> au;
	annexb(au, slice(sps, false, ref_idc, 0, frame_num, frame_num * 2, body));
	return au;
}

void test_bits()
{
	std::vector<uint8_t> e;
	h264::escape({0, 0, 1}, e);
	CHECK(e == std::vector<uint8_t>({0, 0, 3, 1}));
	e.clear();
	h264::escape({0, 0, 0, 0}, e);
	CHECK(e == std::vector<uint8_t>({0, 0, 3, 0, 0}));
	e.clear();
	h264::escape({0, 0, 4}, e);
	CHECK(e == std::vector<uint8_t>({0, 0, 4}));

	h264::Sps s;
	h264::Pps p;
	CHECK_OK(h264::parse_sps(kSps.data(), kSps.size(), s));
	CHECK_EQ(s.profile, 66);
	CHECK_EQ(s.level, 30);
	CHECK_EQ(s.width, 720);
	CHECK_EQ(s.height, 336);
	CHECK_EQ(s.mbs(), 45 * 21);
	CHECK(s.frame_mbs_only);
	CHECK_OK(h264::parse_pps(kPps.data(), kPps.size(), p));
	CHECK(!p.cabac);
	CHECK_EQ(p.slice_groups, 1);
	CHECK_OK(h264::skip_supported(s, p));
	CHECK_OK(live::check_decodable(s, 25));
	CHECK(!live::check_decodable(s, 31).empty());
	h264::Sps big = s;
	big.width = 800;
	CHECK(!live::check_decodable(big, 25).empty());
	big = s;
	big.profile = 77;
	CHECK(!live::check_decodable(big, 25).empty());
	CHECK(h264::parse_sps(kPps.data(), kPps.size(), s) != "");
	CHECK(h264::parse_sps(kSps.data(), 6, s) != "");        // cut short

	// a skip slice parses back as what it should be
	CHECK_OK(h264::parse_sps(kSps.data(), kSps.size(), s));
	unsigned fn = 3 % (1u << s.log2_max_frame_num);
	std::vector<uint8_t> skip = h264::skip_slice(s, p, fn, 0);
	h264::Slice sl;
	CHECK_OK(h264::parse_slice(skip.data(), skip.size(), s, p, sl));
	CHECK_EQ(sl.nal_type, 1);
	CHECK_EQ(sl.ref_idc, 0);
	CHECK_EQ(sl.first_mb, 0);
	CHECK_EQ(sl.slice_type, 5);
	CHECK_EQ(sl.frame_num, fn);
	CHECK(skip.size() < 16);

	// POC type 0: the skip carries its POC LSB
	std::vector<uint8_t> sp0 = sps_poc0();
	h264::Sps s0;
	CHECK_OK(h264::parse_sps(sp0.data(), sp0.size(), s0));
	CHECK_EQ(s0.poc_type, 0);
	CHECK_EQ(s0.log2_max_poc_lsb, 6);
	CHECK_EQ(s0.width, 176);
	CHECK_EQ(s0.height, 144);
	skip = h264::skip_slice(s0, p, 7, 21);
	CHECK_OK(h264::parse_slice(skip.data(), skip.size(), s0, p, sl));
	CHECK_EQ(sl.frame_num, 7);
	CHECK_EQ(sl.poc_lsb, 21);

	// the test slice writer, read back
	std::vector<uint8_t> idr = slice(s, true, 3, 0, 0, 0, 10);
	CHECK_OK(h264::parse_slice(idr.data(), idr.size(), s, p, sl));
	CHECK(sl.idr());
	CHECK_EQ(sl.ref_idc, 3);
}

void test_split()
{
	h264::Sps s;
	h264::parse_sps(kSps.data(), kSps.size(), s);
	std::vector<uint8_t> st = idr_au(s, kSps, kPps, 20);
	size_t au1 = st.size();
	std::vector<uint8_t> p1 = p_au(s, 1, 20);
	st.insert(st.end(), p1.begin(), p1.end());
	annexb(st, slice(s, false, 2, 5, 1, 0, 20));     // a second slice of frame 1
	size_t au2 = st.size();
	std::vector<uint8_t> p2 = p_au(s, 2, 20);
	st.insert(st.end(), p2.begin(), p2.end());

	std::vector<h264::Nal> nals = h264::split_annexb(st.data(), st.size());
	CHECK_EQ(nals.size(), 7);
	CHECK_EQ(nals[0].type(), h264::kAud);
	CHECK_EQ(nals[1].type(), h264::kSps);
	CHECK_EQ(nals[1].n, kSps.size());
	CHECK_EQ(nals[3].type(), h264::kIdr);

	std::vector<h264::Range> aus = h264::split_access_units(st.data(), st.size());
	CHECK_EQ(aus.size(), 3);
	if (aus.size() == 3) {
		CHECK_EQ(aus[0].begin, 0);
		CHECK_EQ(aus[0].end, au1);
		CHECK_EQ(aus[1].begin, au1);
		CHECK_EQ(aus[1].end, au2);
		CHECK_EQ(aus[2].end, st.size());
	}
}

void test_header()
{
	std::vector<uint8_t> hdr;
	CHECK_OK(csmp4::build_header(kSps, kPps, 25, 28672, 15000, hdr));
	std::vector<uint8_t> file = hdr;
	file.insert(file.end(), 28672, 0);
	csmp4::Info m;
	CHECK_OK(csmp4::parse(file.data(), file.size(), file.size(), m));
	CHECK_EQ(m.data_off, hdr.size());
	CHECK_EQ(m.sample_size, 28672);
	CHECK_EQ(m.declared, 15000);
	CHECK_EQ(m.present, 1);
	CHECK(m.fps() > 24.99 && m.fps() < 25.01);
	CHECK_EQ(hdr[hdr.size() - 8], 0);                       // mdat size 0
	CHECK(hdr.size() < 1024);

	CHECK_OK(csmp4::build_header(kSps, kPps, 25, 4096, 10, hdr, true));
	file = hdr;
	file.insert(file.end(), 10 * 4096, 0);
	CHECK_OK(csmp4::parse(file.data(), file.size(), file.size(), m));
	CHECK_EQ(m.present, 10);

	uint32_t most = csmp4::max_frames(28672);
	CHECK((uint64_t)most * 28672 + 4096 < ((uint64_t)1 << 32));
	CHECK(most > 25 * 3600);                                // over an hour at 25 fps
	CHECK_OK(csmp4::build_header(kSps, kPps, 25, 28672, most, hdr));
	CHECK(!csmp4::build_header(kSps, kPps, 25, 65536, 70000, hdr).empty());   // 4.6 GB
	CHECK(!csmp4::build_header(kPps, kSps, 25, 4096, 10, hdr).empty());       // swapped
	CHECK(!csmp4::build_header(kSps, kPps, 0, 4096, 10, hdr).empty());
}

void test_sample()
{
	h264::Sps s;
	h264::parse_sps(kSps.data(), kSps.size(), s);
	std::vector<uint8_t> au = idr_au(s, kSps, kPps, 100);
	std::vector<uint8_t> out;
	CHECK_OK(csmp4::make_sample(au.data(), au.size(), 4096, out));
	CHECK_EQ(out.size(), 4096);
	// SPS first (the AUD is dropped), with its length
	CHECK_EQ(out[3], kSps.size());
	CHECK_EQ(out[4], 0x67);
	size_t used = 4 + kSps.size() + 4 + kPps.size() + 4 + slice(s, true, 3, 0, 0, 0, 100).size();
	CHECK_EQ(out[used + 4], h264::kFiller);
	uint32_t flen = (uint32_t)out[used] << 24 | out[used + 1] << 16 | out[used + 2] << 8 | out[used + 3];
	CHECK_EQ(flen, 4096 - used - 4);
	CHECK_EQ(out[4095], 0x80);
	CHECK_EQ(out[4094], 0xff);

	// exactly S: no filler; 1..5 bytes short of S: can't be padded
	CHECK_OK(csmp4::make_sample(au.data(), au.size(), (uint32_t)used, out));
	CHECK_EQ(out.size(), used);
	CHECK(!csmp4::make_sample(au.data(), au.size(), (uint32_t)used + 3, out).empty());
	CHECK(!csmp4::make_sample(au.data(), au.size(), (uint32_t)used - 1, out).empty());
	CHECK_OK(csmp4::make_sample(au.data(), au.size(), (uint32_t)used + 6, out));
	const uint8_t aud[] = {0, 0, 1, 9, 0xf0};
	CHECK(!csmp4::make_sample(aud, sizeof(aud), 4096, out).empty());     // nothing left

	csmp4::filler_sample(4096, out);
	CHECK_EQ(out.size(), 4096);
	CHECK_EQ(out[2], (4092 >> 8) & 0xff);
	CHECK_EQ(out[4], h264::kFiller);
}

live::Config cfg(uint32_t s, uint32_t declared, bool fill, unsigned ring)
{
	live::Config c;
	c.sps = kSps;
	c.pps = kPps;
	c.fps = 25;
	c.sample_size = s;
	c.declared = declared;
	c.fill = fill;
	c.ring_frames = ring;
	return c;
}

uint8_t frame_byte(live::Stream &st, uint64_t k, uint32_t j)
{
	uint8_t b = 0;
	st.read(st.data_off() + k * st.sample_size() + j, &b, 1);
	return b;
}

void test_stream()
{
	live::Stream st;
	h264::Sps s;
	h264::parse_sps(kSps.data(), kSps.size(), s);
	std::string why;
	CHECK(!st.configure(cfg(1000, 10, false, 8)).empty());              // S too small
	CHECK_OK(st.configure(cfg(8192, 20, false, 8)));
	CHECK_EQ(st.declared(), 20);
	CHECK_EQ(st.total(), st.data_off() + 20 * 8192);
	CHECK_EQ(st.available(), st.data_off());                            // the header only

	std::vector<uint8_t> f = p_au(s, 1);
	CHECK((int)st.push(f.data(), f.size(), 0) == (int)live::Push::WaitIdr);
	f = idr_au(s, kSps, kPps);
	CHECK((int)st.push(f.data(), f.size(), 0, &why) == (int)live::Push::Ok);
	CHECK(st.started());
	CHECK_EQ(st.frames(), 1);
	CHECK_EQ(st.available(), st.data_off() + 8192);
	f = p_au(s, 1);
	CHECK((int)st.push(f.data(), f.size(), 40) == (int)live::Push::Ok);

	uint8_t head[8];
	CHECK_EQ(st.read(0, head, 8), 8);
	CHECK(std::memcmp(head + 4, "ftyp", 4) == 0);
	CHECK_EQ(frame_byte(st, 0, 4), 0x67);                               // frame 0: SPS first
	CHECK_EQ(frame_byte(st, 1, 4), 0x41);                               // frame 1: the P slice
	CHECK_EQ(st.push_ms(0), 0);
	CHECK_EQ(st.push_ms(1), 40);
	CHECK_EQ(st.push_ms(2), -1);

	// too big: dropped; it was a reference frame, so up to the next IDR too
	f = p_au(s, 2, 9000);
	CHECK((int)st.push(f.data(), f.size(), 80) == (int)live::Push::TooBig);
	CHECK(st.sync_wanted());
	CHECK(!st.sync_wanted());
	f = p_au(s, 3);
	CHECK((int)st.push(f.data(), f.size(), 120) == (int)live::Push::WaitIdr);
	f = idr_au(s, kSps, kPps);
	CHECK((int)st.push(f.data(), f.size(), 160) == (int)live::Push::Ok);
	// a too-big non-reference frame: nothing else is dropped
	f = p_au(s, 1, 9000, 0);
	CHECK((int)st.push(f.data(), f.size(), 200) == (int)live::Push::TooBig);
	CHECK(!st.sync_wanted());
	f = p_au(s, 1);
	CHECK((int)st.push(f.data(), f.size(), 240) == (int)live::Push::Ok);

	// in-band SPS that differs from the config
	std::vector<uint8_t> sps2 = kSps;
	sps2[3] = 0x1f;                                                     // level 3.1
	f = idr_au(s, sps2, kPps);
	CHECK((int)st.push(f.data(), f.size(), 280) == (int)live::Push::ConfigChanged);
	CHECK((int)st.push(nullptr, 0, 280) == (int)live::Push::Bad);

	live::Stats ss = st.stats();
	CHECK_EQ(ss.real, 4);
	CHECK_EQ(ss.idr, 2);
	CHECK_EQ(ss.too_big, 2);
	CHECK_EQ(ss.wait_idr, 2);
	CHECK_EQ(ss.config_changed, 1);

	// ring of 8: frame 0 is gone after 9 more
	for (unsigned k = 2; k < 11; k++) {
		f = p_au(s, k);
		CHECK((int)st.push(f.data(), f.size(), 300 + k) == (int)live::Push::Ok);
	}
	CHECK_EQ(st.frames(), 13);
	CHECK_EQ(frame_byte(st, 0, 4), h264::kFiller);                     // evicted: filler
	CHECK_EQ(st.stats().evicted_reads, 1);
	CHECK_EQ(st.push_ms(0), -1);
	CHECK_EQ(frame_byte(st, 12, 4), 0x41);
	CHECK_EQ(frame_byte(st, 15, 4), h264::kFiller);                    // not there yet: filler
	uint8_t b;
	CHECK_EQ(st.read(st.total(), &b, 1), 0);                            // past the end

	// declared 20: full
	for (unsigned k = 11; k < 18; k++) {
		f = p_au(s, k);
		st.push(f.data(), f.size(), 400 + k);
	}
	CHECK_EQ(st.frames(), 20);
	f = p_au(s, 18);
	CHECK((int)st.push(f.data(), f.size(), 500) == (int)live::Push::Full);
	st.finish();
	CHECK(st.finished());
}

void test_fill()
{
	h264::Sps s;
	h264::parse_sps(kSps.data(), kSps.size(), s);
	live::Stream st;
	CHECK_OK(st.configure(cfg(8192, 0, true, 64)));
	CHECK(st.skips_possible());
	CHECK_EQ(st.fill(1000), 0);                                         // not started
	std::vector<uint8_t> f = idr_au(s, kSps, kPps);
	st.push(f.data(), f.size(), 0);
	f = p_au(s, 1);
	st.push(f.data(), f.size(), 40);
	f = p_au(s, 2);
	st.push(f.data(), f.size(), 80);

	CHECK_EQ(st.fill(100), 0);           // on time: 3 frames due
	CHECK_EQ(st.fill(119), 0);           // idle 39 ms < one period
	CHECK_EQ(st.fill(200), 2);           // 6 due: up to 5
	CHECK_EQ(st.frames(), 5);
	CHECK_EQ(st.stats().gaps, 1);
	// the skip: one non-reference P slice, frame_num = last reference + 1
	h264::Pps p;
	h264::parse_pps(kPps.data(), kPps.size(), p);
	std::vector<uint8_t> smp(8192);
	st.read(st.data_off() + 3 * 8192, smp.data(), smp.size());
	uint32_t len = (uint32_t)smp[0] << 24 | smp[1] << 16 | smp[2] << 8 | smp[3];
	CHECK(len < 32);
	h264::Slice sl;
	CHECK_OK(h264::parse_slice(smp.data() + 4, len, s, p, sl));
	CHECK_EQ(sl.ref_idc, 0);
	CHECK_EQ(sl.frame_num, 3);
	CHECK_EQ(sl.slice_type, 5);
	CHECK_EQ(smp[4 + len + 4], h264::kFiller);
	CHECK_EQ(st.push_ms(3), 200);

	CHECK_EQ(st.fill(220), 0);           // idle since the skips
	CHECK_EQ(st.fill(240), 1);           // 7 due
	f = p_au(s, 3);
	CHECK((int)st.push(f.data(), f.size(), 250) == (int)live::Push::Ok);
	CHECK_EQ(st.frames(), 7);
	CHECK_EQ(st.fill(400), 3);           // 11 due: up to 10
	CHECK_EQ(st.stats().gaps, 2);
	CHECK_EQ(st.stats().skips, 6);

	// a sender on time is never filled
	live::Stream on;
	on.configure(cfg(8192, 0, true, 64));
	for (unsigned k = 0; k < 50; k++) {
		f = k == 0 ? idr_au(s, kSps, kPps) : p_au(s, k % 16);
		on.push(f.data(), f.size(), k * 40);
		on.fill(k * 40 + 15);
		on.fill(k * 40 + 39);
	}
	CHECK_EQ(on.stats().skips, 0);

	// a 15 fps sender is made up to 25, never ahead of the clock
	live::Stream slow;
	slow.configure(cfg(8192, 0, true, 128));
	unsigned next = 0, ahead = 0;
	for (unsigned t = 0; t <= 4000; t += 10) {
		if (t >= next * 1000 / 15) {
			f = next == 0 ? idr_au(s, kSps, kPps) : p_au(s, next % 16);
			slow.push(f.data(), f.size(), t);
			next++;
		}
		slow.fill(t);
		if (slow.frames() > t * 25 / 1000 + 1)
			ahead++;
	}
	CHECK_EQ(ahead, 0);
	CHECK(slow.frames() >= 97 && slow.frames() <= 101);
	CHECK(slow.stats().skips >= 35);

	// no fill while waiting for an IDR, or when off
	live::Stream off;
	off.configure(cfg(8192, 0, false, 64));
	f = idr_au(s, kSps, kPps);
	off.push(f.data(), f.size(), 0);
	CHECK_EQ(off.fill(1000), 0);
	live::Stream drop;
	drop.configure(cfg(8192, 0, true, 64));
	drop.push(f.data(), f.size(), 0);
	f = p_au(s, 1, 9000);
	drop.push(f.data(), f.size(), 40);
	CHECK_EQ(drop.fill(1000), 0);
}

void test_burst()
{
	plan::Schedule b(25.0, {}, 5);
	CHECK_EQ(b.release_ms(0), 0);
	CHECK_EQ(b.release_ms(4), 0);
	CHECK_EQ(b.release_ms(5), 200);
	CHECK_EQ(b.release_ms(9), 200);
	CHECK_EQ(b.released(0), 5);
	CHECK_EQ(b.released(199), 5);
	CHECK_EQ(b.released(200), 10);

	plan::Schedule bs(25.0, {{7, 1000}}, 5);
	CHECK_EQ(bs.release_ms(6), 200);
	CHECK_EQ(bs.release_ms(7), 1200);
	CHECK_EQ(bs.released(500), 7);
	CHECK_EQ(bs.released(1200), 10);

	plan::Schedule slow(15.0, {});
	CHECK_EQ(slow.release_ms(15), 1000);
	CHECK_EQ(slow.released(1000), 16);
}

// ---- with a real clip (tests/check_live.py prepare) ----

const char kDir[] = "build/test_live.d";

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0);
	bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	return ok;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &data)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	return std::fclose(f) == 0 && ok;
}

void test_with_clip()
{
	std::vector<uint8_t> clip, ref;
	if (!read_file(std::string(kDir) + "/in.h264", clip) ||
	    !read_file(std::string(kDir) + "/ref.mp4", ref)) {
		std::printf("test_live: no %s/in.h264 + ref.mp4: clip checks skipped "
		            "(tests/check_live.py prepare)\n", kDir);
		return;
	}
	std::vector<h264::Range> aus = h264::split_access_units(clip.data(), clip.size());
	std::vector<uint8_t> sps, pps;
	size_t biggest = 0;
	for (const h264::Range &r : aus) {
		size_t bytes = 0;
		for (const h264::Nal &n : h264::split_annexb(clip.data() + r.begin, r.end - r.begin)) {
			if (n.type() == h264::kSps && sps.empty())
				sps.assign(n.p, n.p + n.n);
			if (n.type() == h264::kPps && pps.empty())
				pps.assign(n.p, n.p + n.n);
			if (n.type() != h264::kAud)
				bytes += 4 + n.n;
		}
		biggest = std::max(biggest, bytes);
	}
	CHECK(!sps.empty() && !pps.empty() && aus.size() > 40);
	uint32_t s = (uint32_t)((biggest + csmp4::kFillerMin + 4095) / 4096 * 4096);

	// our header = media/csmp4.py's (declared 600 s), byte for byte
	csmp4::Info m;
	CHECK_OK(csmp4::parse(ref.data(), ref.size(), ref.size(), m));
	CHECK_EQ(m.sample_size, s);
	CHECK_EQ(m.present, aus.size());
	std::vector<uint8_t> hdr;
	CHECK_OK(csmp4::build_header(sps, pps, 25, s, m.declared, hdr));
	CHECK_EQ(hdr.size(), m.data_off);
	CHECK(hdr.size() <= ref.size() && std::memcmp(hdr.data(), ref.data(), hdr.size()) == 0);
	// and the frames: the same samples
	std::vector<uint8_t> smp;
	bool same = true;
	for (size_t k = 0; k < aus.size(); k++) {
		csmp4::make_sample(clip.data() + aus[k].begin, aus[k].end - aus[k].begin, s, smp);
		same = same && std::memcmp(smp.data(), ref.data() + m.data_off + k * s, s) == 0;
	}
	CHECK(same);

	// a stream with a 600 ms gap after frame 29, filled: for ffmpeg to decode
	// (check_live.py check: every skip shows the frame before it)
	live::Config c;
	c.sps = sps;
	c.pps = pps;
	c.fps = 25;
	c.sample_size = s;
	c.declared = 1000;
	c.ring_frames = 1000;
	live::Stream st;
	CHECK_OK(st.configure(c));
	uint64_t t = 0;
	for (size_t k = 0; k < aus.size(); k++) {
		uint64_t due = k * 40 + (k >= 30 ? 600 : 0);
		for (; t < due; t += 10)
			st.fill(t);
		CHECK((int)st.push(clip.data() + aus[k].begin, aus[k].end - aus[k].begin, due) ==
		      (int)live::Push::Ok);
	}
	live::Stats ls = st.stats();
	CHECK(ls.skips >= 12 && ls.skips <= 15);
	CHECK_EQ(ls.real, aus.size());

	std::vector<uint8_t> es, mp4;
	std::string skips;
	CHECK_OK(csmp4::build_header(sps, pps, 25, s, (uint32_t)st.frames(), mp4, true));
	for (uint64_t k = 0; k < st.frames(); k++) {
		st.read(st.data_off() + k * s, smp.data(), s);
		mp4.insert(mp4.end(), smp.begin(), smp.end());
		bool skip = false;
		for (uint32_t at = 0; at + 4 <= s;) {
			uint32_t len = (uint32_t)smp[at] << 24 | smp[at + 1] << 16 | smp[at + 2] << 8 | smp[at + 3];
			if (len == 0 || at + 4 + len > s)
				break;
			uint8_t type = smp[at + 4] & 0x1f;
			if (type == h264::kFiller)
				break;
			if (type == h264::kSlice && (smp[at + 4] >> 5) == 0)
				skip = true;
			std::vector<uint8_t> nal(smp.begin() + at + 4, smp.begin() + at + 4 + len);
			annexb(es, nal);
			at += 4 + len;
		}
		if (skip)
			skips += std::to_string(k) + "\n";
	}
	CHECK(write_file(std::string(kDir) + "/gap.h264", es));
	CHECK(write_file(std::string(kDir) + "/gap.mp4", mp4));
	std::string hdr_line = std::to_string(st.frames()) + "\n" + skips;
	CHECK(write_file(std::string(kDir) + "/gap_skips.txt",
	                 std::vector<uint8_t>(hdr_line.begin(), hdr_line.end())));
	std::printf("test_live: %zu clip frames, header = csmp4.py's (%zu bytes), gap stream: %llu "
	            "frames, %u skips -> %s/gap.*\n",
	            aus.size(), hdr.size(), (unsigned long long)st.frames(), ls.skips, kDir);
}
} // namespace

int main()
{
	test_parse();
	test_parse_rejects();
	test_schedule();
	test_bits();
	test_split();
	test_header();
	test_sample();
	test_stream();
	test_fill();
	test_burst();
	test_with_clip();
	CHECK_DONE();
}

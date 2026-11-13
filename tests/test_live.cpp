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
#include "types.h"
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
	std::vector<u8> b;
	void put8(u32 v) { b.push_back((u8)v); }
	void put16(u32 v) { put8(v >> 8); put8(v); }
	void put32(u32 v) { put16(v >> 16); put16(v & 0xffff); }
	void put64(u64 v) { put32((u32)(v >> 32)); put32((u32)v); }
	void tag(const char *t) { for (int i = 0; i < 4; i++) put8((u8)t[i]); }
	void zeros(int n) { b.insert(b.end(), (usize)n, 0); }
	usize open(const char *t)
	{
		usize at = b.size();
		put32(0);
		tag(t);
		return at;
	}
	void close(usize at)
	{
		u32 n = (u32)(b.size() - at);
		b[at] = (u8)(n >> 24);
		b[at + 1] = (u8)(n >> 16);
		b[at + 2] = (u8)(n >> 8);
		b[at + 3] = (u8)n;
	}
};

struct Spec {
	u32 timescale = 12800, delta = 512;
	u32 size = 4096, declared = 100;
	u32 stts_entries = 1, stsc_per_chunk = 0;   // 0: = declared
	bool co64 = false;
	u32 present = 3;                            // frames written after the header
	int mdat = 0;          // mdat header: 0 exact 32-bit size, 1 size 0, 2 64-bit, 3 wrong size
};

std::vector<u8> make(const Spec &s, u64 *data_off)
{
	Buf f;
	usize a = f.open("ftyp");
	f.tag("isom");
	f.put32(0x200);
	f.tag("isom");
	f.close(a);

	usize moov = f.open("moov");
	usize mvhd = f.open("mvhd");
	f.zeros(100);
	f.close(mvhd);
	usize trak = f.open("trak");
	usize tkhd = f.open("tkhd");
	f.zeros(84);
	f.close(tkhd);
	usize mdia = f.open("mdia");
	usize mdhd = f.open("mdhd");
	f.put32(0);                         // version 0, flags
	f.put32(0);
	f.put32(0);
	f.put32(s.timescale);
	f.put32(s.declared * s.delta);
	f.put32(0x55c40000);
	f.close(mdhd);
	usize minf = f.open("minf");
	usize stbl = f.open("stbl");
	usize stsd = f.open("stsd");
	f.put32(0);
	f.put32(1);
	usize avc1 = f.open("avc1");
	f.zeros(78);
	f.close(avc1);
	f.close(stsd);
	usize stts = f.open("stts");
	f.put32(0);
	f.put32(s.stts_entries);
	for (u32 i = 0; i < s.stts_entries; i++) {
		f.put32(s.declared / s.stts_entries);
		f.put32(s.delta);
	}
	f.close(stts);
	usize stsc = f.open("stsc");
	f.put32(0);
	f.put32(1);
	f.put32(1);
	f.put32(s.stsc_per_chunk ? s.stsc_per_chunk : s.declared);
	f.put32(1);
	f.close(stsc);
	usize stsz = f.open("stsz");
	f.put32(0);
	f.put32(s.size);
	f.put32(s.declared);
	f.close(stsz);
	usize co = f.open(s.co64 ? "co64" : "stco");
	f.put32(0);
	f.put32(1);
	usize stco_value = f.b.size();
	if (s.co64)
		f.put64(0);
	else
		f.put32(0);
	f.close(co);
	f.close(stbl);
	f.close(minf);
	f.close(mdia);
	f.close(trak);
	f.close(moov);

	const u64 payload = (u64)s.declared * s.size;
	if (s.mdat == 2) {
		f.put32(1);                 // 64-bit size, as an older csmp4.py wrote
		f.tag("mdat");
		f.put64(16 + payload);
	} else {
		f.put32(s.mdat == 1 ? 0 : (u32)(8 + payload + (s.mdat == 3 ? s.size : 0)));
		f.tag("mdat");
	}
	*data_off = f.b.size();
	if (!s.co64) {
		f.b[stco_value] = (u8)(*data_off >> 24);
		f.b[stco_value + 1] = (u8)(*data_off >> 16);
		f.b[stco_value + 2] = (u8)(*data_off >> 8);
		f.b[stco_value + 3] = (u8)*data_off;
	}
	for (u32 k = 0; k < s.present; k++)
		f.b.insert(f.b.end(), s.size, (u8)('A' + k));
	return f.b;
}

void test_parse()
{
	Spec s;
	u64 data_off = 0;
	std::vector<u8> file = make(s, &data_off);
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
	err = csmp4::parse(file.data(), (usize)data_off, file.size(), h);
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
	u64 off;
	csmp4::Info m;
	std::vector<u8> f;
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
	CHECK(!csmp4::parse(f.data(), (usize)off, f.size(), m).empty());  // over 4 GB

	f = make(Spec(), &off);
	CHECK(!csmp4::parse(f.data(), (usize)off - 8, f.size(), m).empty()); // no mdat header
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
const std::vector<u8> kSps = {0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0xd0, 0xae,
                                   0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00,
                                   0x00, 0x03, 0x00, 0xc8, 0x3c, 0x58, 0xba, 0x80};
const std::vector<u8> kPps = {0x68, 0xce, 0x3c, 0x80};

struct BitW {
	std::vector<u8> out;
	u8 cur = 0;
	unsigned n = 0;
	void bit(unsigned b)
	{
		cur = (u8)(cur << 1 | (b & 1));
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
std::vector<u8> sps_poc0()
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
	std::vector<u8> nal{0x67};
	h264::escape(w.out, nal);
	return nal;
}

// A slice NAL with a valid header start and `body` bytes of 0xaa after it.
std::vector<u8> slice(const h264::Sps &sps, bool idr, unsigned ref_idc, unsigned first_mb,
                           unsigned frame_num, unsigned poc_lsb, usize body)
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
	std::vector<u8> nal{(u8)(ref_idc << 5 | (idr ? 5 : 1))};
	h264::escape(w.out, nal);
	nal.insert(nal.end(), body, 0xaa);
	return nal;
}

void annexb(std::vector<u8> &out, const std::vector<u8> &nal)
{
	static const u8 sc[4] = {0, 0, 0, 1};
	out.insert(out.end(), sc, sc + 4);
	out.insert(out.end(), nal.begin(), nal.end());
}

std::vector<u8> idr_au(const h264::Sps &sps, const std::vector<u8> &sp,
                            const std::vector<u8> &pp, usize body = 300)
{
	std::vector<u8> au;
	annexb(au, {0x09, 0xf0});        // AUD: dropped
	annexb(au, sp);
	annexb(au, pp);
	annexb(au, slice(sps, true, 3, 0, 0, 0, body));
	return au;
}

std::vector<u8> p_au(const h264::Sps &sps, unsigned frame_num, usize body = 200,
                          unsigned ref_idc = 2)
{
	std::vector<u8> au;
	annexb(au, slice(sps, false, ref_idc, 0, frame_num, frame_num * 2, body));
	return au;
}

void test_bits()
{
	std::vector<u8> e;
	h264::escape({0, 0, 1}, e);
	CHECK(e == std::vector<u8>({0, 0, 3, 1}));
	e.clear();
	h264::escape({0, 0, 0, 0}, e);
	CHECK(e == std::vector<u8>({0, 0, 3, 0, 0}));
	e.clear();
	h264::escape({0, 0, 4}, e);
	CHECK(e == std::vector<u8>({0, 0, 4}));

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
	std::vector<u8> skip = h264::skip_slice(s, p, fn, 0);
	h264::Slice sl;
	CHECK_OK(h264::parse_slice(skip.data(), skip.size(), s, p, sl));
	CHECK_EQ(sl.nal_type, 1);
	CHECK_EQ(sl.ref_idc, 0);
	CHECK_EQ(sl.first_mb, 0);
	CHECK_EQ(sl.slice_type, 5);
	CHECK_EQ(sl.frame_num, fn);
	CHECK(skip.size() < 16);

	// POC type 0: the skip carries its POC LSB
	std::vector<u8> sp0 = sps_poc0();
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
	std::vector<u8> idr = slice(s, true, 3, 0, 0, 0, 10);
	CHECK_OK(h264::parse_slice(idr.data(), idr.size(), s, p, sl));
	CHECK(sl.idr());
	CHECK_EQ(sl.ref_idc, 3);
}

// crop_sps: the picture size and level change, nothing else does
void test_crop()
{
	h264::Sps s, c;
	CHECK_OK(h264::parse_sps(kSps.data(), kSps.size(), s));
	std::vector<u8> out, back;
	CHECK_OK(h264::crop_sps(kSps.data(), kSps.size(), 640, 320, 31, out));
	CHECK_OK(h264::parse_sps(out.data(), out.size(), c));
	CHECK_EQ(c.width, 640);
	CHECK_EQ(c.height, 320);
	CHECK_EQ(c.level, 31);
	CHECK_EQ(c.width_mbs, s.width_mbs);
	CHECK_EQ(c.height_mbs, s.height_mbs);
	CHECK_EQ(c.profile, s.profile);
	CHECK_EQ(c.constraints, s.constraints);
	CHECK_EQ(c.poc_type, s.poc_type);
	CHECK_EQ(c.log2_max_frame_num, s.log2_max_frame_num);
	CHECK_EQ(c.max_ref_frames, s.max_ref_frames);
	CHECK_EQ(out[0], kSps[0]);
	// back to the full picture and the old level: the original, byte for byte
	CHECK_OK(h264::crop_sps(out.data(), out.size(), s.width, s.height, s.level, back));
	CHECK(back == kSps);
	// level 0 keeps it
	CHECK_OK(h264::crop_sps(kSps.data(), kSps.size(), 720, 330, 0, out));
	CHECK_OK(h264::parse_sps(out.data(), out.size(), c));
	CHECK_EQ(c.level, s.level);
	CHECK_EQ(c.height, 330);
	// POC type 0 with its own fields
	std::vector<u8> sp0 = sps_poc0();
	h264::Sps s0, c0;
	CHECK_OK(h264::parse_sps(sp0.data(), sp0.size(), s0));
	CHECK_OK(h264::crop_sps(sp0.data(), sp0.size(), 160, 120, 0, out));
	CHECK_OK(h264::parse_sps(out.data(), out.size(), c0));
	CHECK_EQ(c0.width, 160);
	CHECK_EQ(c0.height, 120);
	CHECK_EQ(c0.log2_max_poc_lsb, s0.log2_max_poc_lsb);
	// refused: bigger than coded, odd, zero, not an SPS
	CHECK(!h264::crop_sps(kSps.data(), kSps.size(), 736, 336, 0, out).empty());
	CHECK(!h264::crop_sps(kSps.data(), kSps.size(), 719, 336, 0, out).empty());
	CHECK(!h264::crop_sps(kSps.data(), kSps.size(), 0, 336, 0, out).empty());
	CHECK(!h264::crop_sps(kPps.data(), kPps.size(), 640, 320, 0, out).empty());

	// a whole access unit: the SPS is rewritten, the rest copied
	std::vector<u8> au = idr_au(s, kSps, kPps, 50), au2;
	CHECK_OK(h264::crop_sps_annexb(au.data(), au.size(), 640, 320, 30, au2));
	std::vector<h264::Nal> a = h264::split_annexb(au.data(), au.size());
	std::vector<h264::Nal> b = h264::split_annexb(au2.data(), au2.size());
	CHECK_EQ(a.size(), b.size());
	for (usize i = 0; i < a.size() && i < b.size(); i++) {
		CHECK_EQ(a[i].type(), b[i].type());
		if (a[i].type() == h264::kSps) {
			CHECK_OK(h264::parse_sps(b[i].p, b[i].n, c));
			CHECK_EQ(c.width, 640);
		} else {
			CHECK(a[i].n == b[i].n && std::memcmp(a[i].p, b[i].p, a[i].n) == 0);
		}
	}
	// what the DM642 gets for 800x480: a 720x480 Level 3 stream
	h264::Sps wide = s;
	wide.width = 720;
	wide.height = 480;
	CHECK_OK(live::check_decodable(wide, 25));
}

void test_split()
{
	h264::Sps s;
	h264::parse_sps(kSps.data(), kSps.size(), s);
	std::vector<u8> st = idr_au(s, kSps, kPps, 20);
	usize au1 = st.size();
	std::vector<u8> p1 = p_au(s, 1, 20);
	st.insert(st.end(), p1.begin(), p1.end());
	annexb(st, slice(s, false, 2, 5, 1, 0, 20));     // a second slice of frame 1
	usize au2 = st.size();
	std::vector<u8> p2 = p_au(s, 2, 20);
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
	std::vector<u8> hdr;
	CHECK_OK(csmp4::build_header(kSps, kPps, 25, 28672, 15000, hdr));
	std::vector<u8> file = hdr;
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

	u32 most = csmp4::max_frames(28672);
	CHECK((u64)most * 28672 + 4096 < ((u64)1 << 32));
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
	std::vector<u8> au = idr_au(s, kSps, kPps, 100);
	std::vector<u8> out;
	CHECK_OK(csmp4::make_sample(au.data(), au.size(), 4096, out));
	CHECK_EQ(out.size(), 4096);
	// SPS first (the AUD is dropped), with its length
	CHECK_EQ(out[3], kSps.size());
	CHECK_EQ(out[4], 0x67);
	usize used = 4 + kSps.size() + 4 + kPps.size() + 4 + slice(s, true, 3, 0, 0, 0, 100).size();
	CHECK_EQ(out[used + 4], h264::kFiller);
	u32 flen = (u32)out[used] << 24 | out[used + 1] << 16 | out[used + 2] << 8 | out[used + 3];
	CHECK_EQ(flen, 4096 - used - 4);
	CHECK_EQ(out[4095], 0x80);
	CHECK_EQ(out[4094], 0xff);

	// exactly S: no filler; 1..5 bytes short of S: can't be padded
	CHECK_OK(csmp4::make_sample(au.data(), au.size(), (u32)used, out));
	CHECK_EQ(out.size(), used);
	CHECK(!csmp4::make_sample(au.data(), au.size(), (u32)used + 3, out).empty());
	CHECK(!csmp4::make_sample(au.data(), au.size(), (u32)used - 1, out).empty());
	CHECK_OK(csmp4::make_sample(au.data(), au.size(), (u32)used + 6, out));
	const u8 aud[] = {0, 0, 1, 9, 0xf0};
	CHECK(!csmp4::make_sample(aud, sizeof(aud), 4096, out).empty());     // nothing left

	csmp4::filler_sample(4096, out);
	CHECK_EQ(out.size(), 4096);
	CHECK_EQ(out[2], (4092 >> 8) & 0xff);
	CHECK_EQ(out[4], h264::kFiller);
}

live::Config cfg(u32 s, u32 declared, bool fill, unsigned ring)
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

u8 frame_byte(live::Stream &st, u64 k, u32 j)
{
	u8 b = 0;
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

	std::vector<u8> f = p_au(s, 1);
	CHECK((int)st.push(f.data(), f.size(), 0) == (int)live::Push::WaitIdr);
	f = idr_au(s, kSps, kPps);
	CHECK((int)st.push(f.data(), f.size(), 0, &why) == (int)live::Push::Ok);
	CHECK(st.started());
	CHECK_EQ(st.frames(), 1);
	CHECK_EQ(st.available(), st.data_off() + 8192);
	f = p_au(s, 1);
	CHECK((int)st.push(f.data(), f.size(), 40) == (int)live::Push::Ok);

	u8 head[8];
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
	std::vector<u8> sps2 = kSps;
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
	u8 b;
	CHECK_EQ(st.read(st.total(), &b, 1), 0);                            // past the end
	// what a read served as filler, frame by frame: 0..4 gone (ring of 8 of
	// 13), 5..12 kept, 13..15 not pushed yet
	{
		std::vector<u8> span(16 * 8192);
		live::Stream::ReadInfo info;
		CHECK_EQ(st.read(st.data_off(), span.data(), span.size(), &info), span.size());
		CHECK_EQ(info.evicted, 5);
		CHECK_EQ(info.ahead, 3);
		CHECK_EQ(st.stats().evicted_reads, 6);
		live::Stream::ReadInfo none;
		st.read(st.data_off() + 6 * 8192 + 100, span.data(), 8192, &none);   // 6..7, kept
		CHECK_EQ(none.evicted, 0);
		CHECK_EQ(none.ahead, 0);
	}

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
	std::vector<u8> f = idr_au(s, kSps, kPps);
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
	std::vector<u8> smp(8192);
	st.read(st.data_off() + 3 * 8192, smp.data(), smp.size());
	u32 len = (u32)smp[0] << 24 | smp[1] << 16 | smp[2] << 8 | smp[3];
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

// the clock cap: a 26 fps sender against a 25 fps stream (the 10.11 car
// run's drift), and a burst after a stall
void test_shed()
{
	h264::Sps s;
	h264::parse_sps(kSps.data(), kSps.size(), s);
	std::vector<u8> f;

	// off (0): the stream just gets further ahead
	live::Stream off;
	off.configure(cfg(8192, 0, true, 128));
	for (unsigned k = 0; k < 26 * 60; k++) {
		f = k == 0 ? idr_au(s, kSps, kPps) : p_au(s, k % 16);
		off.push(f.data(), f.size(), k * 1000 / 26);
	}
	CHECK(off.stats().ahead_ms_max > 2000);
	CHECK_EQ(off.stats().shed, 0);

	// on: never more than the cap (+ a frame) ahead; each time it sheds,
	// an IDR is asked for once it is back on the clock, and the stream
	// goes on from it
	live::Config c = cfg(8192, 0, true, 128);
	c.shed_ahead_ms = 500;
	live::Stream on;
	CHECK_OK(on.configure(c));
	unsigned sync_asks = 0, idr_sent = 0, max_ahead = 0;
	bool want_idr = false;
	for (unsigned k = 0; k < 26 * 60; k++) {
		u64 t = k * 1000 / 26;
		bool idr = k == 0 || want_idr;
		f = idr ? idr_au(s, kSps, kPps) : p_au(s, k % 16);
		live::Push p = on.push(f.data(), f.size(), t);
		if (idr && p == live::Push::Ok) {
			want_idr = false;
			idr_sent++;
		}
		CHECK(p == live::Push::Ok || p == live::Push::Shed || p == live::Push::WaitIdr);
		if (on.sync_wanted()) {
			sync_asks++;
			want_idr = true;                 // the phone's next frame is an IDR
		}
		on.fill(t + 1);
		i64 ahead = (i64)(on.frames() * 40) - (i64)t;
		if (ahead > (i64)max_ahead)
			max_ahead = (unsigned)ahead;
	}
	live::Stats st = on.stats();
	CHECK(st.ahead_ms_max <= 540);
	CHECK(max_ahead <= 580);
	CHECK(st.sheds >= 2 && st.sheds <= 4);  // ~1 frame/s too many: every ~13 s
	CHECK_EQ(sync_asks, st.sheds);
	CHECK_EQ(idr_sent, st.sheds + 1);
	CHECK(st.shed >= 12 * st.sheds);         // down from 500 ms ahead to on time

	// a 1 s stall, then the 25 queued frames at once: shed, not played late
	live::Stream burst;
	burst.configure(c);
	unsigned k = 0;
	for (; k < 25; k++) {
		f = k == 0 ? idr_au(s, kSps, kPps) : p_au(s, k % 16);
		burst.push(f.data(), f.size(), k * 40);
	}
	burst.fill(1990);                                // the stall: filled up to the clock
	for (unsigned j = 0; j < 25; j++, k++) {
		f = p_au(s, k % 16);
		burst.push(f.data(), f.size(), 2000);
	}
	CHECK_EQ(burst.stats().sheds, 1);
	CHECK(burst.stats().shed >= 10);
	CHECK(burst.stats().ahead_ms_max <= 540);
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
constexpr unsigned kCropW = 288, kCropH = 208;   // check_live.py's CROP

bool read_file(const std::string &path, std::vector<u8> &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (usize)n : 0);
	bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	return ok;
}

bool write_file(const std::string &path, const std::vector<u8> &data)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	return std::fclose(f) == 0 && ok;
}

void test_with_clip()
{
	std::vector<u8> clip, ref;
	if (!read_file(std::string(kDir) + "/in.h264", clip) ||
	    !read_file(std::string(kDir) + "/ref.mp4", ref)) {
		std::printf("test_live: no %s/in.h264 + ref.mp4: clip checks skipped "
		            "(tests/check_live.py prepare)\n", kDir);
		return;
	}
	std::vector<h264::Range> aus = h264::split_access_units(clip.data(), clip.size());
	std::vector<u8> sps, pps;
	usize biggest = 0;
	for (const h264::Range &r : aus) {
		usize bytes = 0;
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
	u32 s = (u32)((biggest + csmp4::kFillerMin + 4095) / 4096 * 4096);

	// our header = media/csmp4.py's (declared 600 s), byte for byte
	csmp4::Info m;
	CHECK_OK(csmp4::parse(ref.data(), ref.size(), ref.size(), m));
	CHECK_EQ(m.sample_size, s);
	CHECK_EQ(m.present, aus.size());
	std::vector<u8> hdr;
	CHECK_OK(csmp4::build_header(sps, pps, 25, s, m.declared, hdr));
	CHECK_EQ(hdr.size(), m.data_off);
	CHECK(hdr.size() <= ref.size() && std::memcmp(hdr.data(), ref.data(), hdr.size()) == 0);
	// and the frames: the same samples
	std::vector<u8> smp;
	bool same = true;
	for (usize k = 0; k < aus.size(); k++) {
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
	u64 t = 0;
	for (usize k = 0; k < aus.size(); k++) {
		u64 due = k * 40 + (k >= 30 ? 600 : 0);
		for (; t < due; t += 10)
			st.fill(t);
		CHECK((int)st.push(clip.data() + aus[k].begin, aus[k].end - aus[k].begin, due) ==
		      (int)live::Push::Ok);
	}
	live::Stats ls = st.stats();
	CHECK(ls.skips >= 12 && ls.skips <= 15);
	CHECK_EQ(ls.real, aus.size());

	std::vector<u8> es, mp4;
	std::string skips;
	CHECK_OK(csmp4::build_header(sps, pps, 25, s, (u32)st.frames(), mp4, true));
	for (u64 k = 0; k < st.frames(); k++) {
		st.read(st.data_off() + k * s, smp.data(), s);
		mp4.insert(mp4.end(), smp.begin(), smp.end());
		bool skip = false;
		for (u32 at = 0; at + 4 <= s;) {
			u32 len = (u32)smp[at] << 24 | smp[at + 1] << 16 | smp[at + 2] << 8 | smp[at + 3];
			if (len == 0 || at + 4 + len > s)
				break;
			u8 type = smp[at + 4] & 0x1f;
			if (type == h264::kFiller)
				break;
			if (type == h264::kSlice && (smp[at + 4] >> 5) == 0)
				skip = true;
			std::vector<u8> nal(smp.begin() + at + 4, smp.begin() + at + 4 + len);
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
	                 std::vector<u8>(hdr_line.begin(), hdr_line.end())));
	std::printf("test_live: %zu clip frames, header = csmp4.py's (%zu bytes), gap stream: %llu "
	            "frames, %u skips -> %s/gap.*\n",
	            aus.size(), hdr.size(), (unsigned long long)st.frames(), ls.skips, kDir);

	// the clip with its SPS cropped to the top-left 288x208 (hello10's
	// 800 -> 720), raw and as a CS-MP4: check_live.py decodes both and
	// compares them with ffmpeg's own crop of the original
	std::vector<u8> crop_es, crop_mp4, au, csps;
	CHECK_OK(h264::crop_sps(sps.data(), sps.size(), kCropW, kCropH, 30, csps));
	h264::Sps cs;
	CHECK_OK(h264::parse_sps(csps.data(), csps.size(), cs));
	CHECK_EQ(cs.width, kCropW);
	CHECK_EQ(cs.height, kCropH);
	CHECK_OK(csmp4::build_header(csps, pps, 25, s, (u32)aus.size(), crop_mp4, true));
	for (const h264::Range &r : aus) {
		CHECK_OK(h264::crop_sps_annexb(clip.data() + r.begin, r.end - r.begin, kCropW, kCropH, 30, au));
		crop_es.insert(crop_es.end(), au.begin(), au.end());
		CHECK_OK(csmp4::make_sample(au.data(), au.size(), s, smp));
		crop_mp4.insert(crop_mp4.end(), smp.begin(), smp.end());
	}
	CHECK(write_file(std::string(kDir) + "/crop.h264", crop_es));
	CHECK(write_file(std::string(kDir) + "/crop.mp4", crop_mp4));
}
} // namespace

int main()
{
	test_parse();
	test_parse_rejects();
	test_schedule();
	test_bits();
	test_crop();
	test_split();
	test_header();
	test_sample();
	test_stream();
	test_fill();
	test_shed();
	test_burst();
	test_with_clip();
	CHECK_DONE();
}

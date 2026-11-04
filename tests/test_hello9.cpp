// test_hello9.cpp - hello9's stream plan (src/hello9/stream_plan.cpp) on
// the build host: the CS-MP4 header parser, the frame release schedule and
// the served-byte -> source-byte mapping.
#include "check.h"
#include "stream_plan.h"

#include <cstdint>
#include <string>
#include <vector>

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
		f.u32(1);                 // 64-bit size, as csmp4.py wrote until 03.11
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
	plan::CsMp4 m;
	std::string err = plan::parse(file.data(), file.size(), file.size(), m);

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
	plan::CsMp4 h;
	err = plan::parse(file.data(), (size_t)data_off, file.size(), h);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(h.present, 3);

	// a partial last frame does not count
	err = plan::parse(file.data(), file.size(), file.size() - 1, h);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(h.present, 2);

	// mdat size 0 (to the end of the file): an over-declared stream
	s.mdat = 1;
	file = make(s, &data_off);
	err = plan::parse(file.data(), file.size(), file.size(), m);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(m.data_off, data_off);
	CHECK_EQ(m.present, 3);
}

void test_parse_rejects()
{
	uint64_t off;
	plan::CsMp4 m;
	std::vector<uint8_t> f;
	Spec s;

	s.size = 0;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // per-sample sizes

	s = Spec();
	s.stts_entries = 2;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // two durations

	s = Spec();
	s.stsc_per_chunk = 10;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // several chunks

	s = Spec();
	s.co64 = true;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // co64

	s = Spec();
	s.present = 0;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // no frames

	s = Spec();
	s.mdat = 2;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // 64-bit mdat size

	s = Spec();
	s.mdat = 3;
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // mdat past the frames

	s = Spec();
	s.size = 65536;
	s.declared = 70000;                                                // 4.6 GB
	f = make(s, &off);
	CHECK(!plan::parse(f.data(), (size_t)off, f.size(), m).empty());  // over 4 GB

	f = make(Spec(), &off);
	CHECK(!plan::parse(f.data(), (size_t)off - 8, f.size(), m).empty()); // no mdat header
	CHECK(!plan::parse(f.data(), 40, f.size(), m).empty());           // truncated moov
	f[4] = 'x';
	CHECK(!plan::parse(f.data(), f.size(), f.size(), m).empty());     // not ftyp
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

void test_map()
{
	plan::CsMp4 m;
	m.data_off = 1000;
	m.sample_size = 100;
	m.declared = 50;
	m.present = 3;

	plan::Span sp = plan::map(m, 0);
	CHECK_EQ(sp.src_off, 0);
	CHECK_EQ(sp.len, 1000);                 // the header, to the first frame
	sp = plan::map(m, 990);
	CHECK_EQ(sp.src_off, 990);
	CHECK_EQ(sp.len, 10);
	sp = plan::map(m, 1000);                // frame 0
	CHECK_EQ(sp.src_off, 1000);
	CHECK_EQ(sp.len, 100);
	sp = plan::map(m, 1250);                // frame 2, byte 50
	CHECK_EQ(sp.src_off, 1250);
	CHECK_EQ(sp.len, 50);
	sp = plan::map(m, 1310);                // frame 3 = source frame 0, byte 10
	CHECK_EQ(sp.src_off, 1010);
	CHECK_EQ(sp.len, 90);
	sp = plan::map(m, 1000 + 49 * 100);     // frame 49 = source frame 1
	CHECK_EQ(sp.src_off, 1100);

	CHECK_EQ(plan::frames_needed(m, 0), 0);
	CHECK_EQ(plan::frames_needed(m, 1000), 0);
	CHECK_EQ(plan::frames_needed(m, 1001), 1);
	CHECK_EQ(plan::frames_needed(m, 1100), 1);
	CHECK_EQ(plan::frames_needed(m, 1101), 2);
}

} // namespace

int main()
{
	test_parse();
	test_parse_rejects();
	test_schedule();
	test_map();
	CHECK_DONE();
}

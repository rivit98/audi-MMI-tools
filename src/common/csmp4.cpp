// csmp4.cpp - see csmp4.h.
#include "csmp4.h"

#include "h264.h"

#include <cstring>
#include <utility>

namespace csmp4 {

namespace {

uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

bool is(const uint8_t *type, const char *name)
{
	return std::memcmp(type, name, 4) == 0;
}

// ---- parse ----

struct Found {
	bool mdhd = false, stts = false, stsc = false, stsz = false, stco = false, co64 = false;
	uint32_t timescale = 0, delta = 0, stts_n = 0, stsc_n = 0, per_chunk = 0, first_chunk = 0;
	uint32_t size = 0, count = 0, stco_n = 0;
	uint64_t chunk_off = 0;
};

// Walk the boxes in [p, end); descend into the containers on the path to
// the sample table. Returns false if a box runs past end.
bool walk(const uint8_t *p, const uint8_t *end, Found &f)
{
	while (p + 8 <= end) {
		uint64_t size = be32(p);
		const uint8_t *type = p + 4, *body = p + 8;

		if (size == 1) {
			if (p + 16 > end)
				return false;
			size = be64(p + 8);
			body = p + 16;
		} else if (size == 0) {
			size = (uint64_t)(end - p);
		}
		if (size < (uint64_t)(body - p) || size > (uint64_t)(end - p))
			return false;
		const uint8_t *next = p + size;
		const size_t n = (size_t)(next - body);

		if (is(type, "moov") || is(type, "trak") || is(type, "mdia") || is(type, "minf") ||
		    is(type, "stbl")) {
			if (!walk(body, next, f))
				return false;
		} else if (is(type, "mdhd") && !f.mdhd && n >= 24) {
			f.mdhd = true;
			f.timescale = body[0] == 1 ? (n >= 24 ? be32(body + 20) : 0) : be32(body + 12);
		} else if (is(type, "stts") && n >= 8) {
			f.stts = true;
			f.stts_n = be32(body + 4);
			if (f.stts_n >= 1 && n >= 16)
				f.delta = be32(body + 12);
		} else if (is(type, "stsc") && n >= 8) {
			f.stsc = true;
			f.stsc_n = be32(body + 4);
			if (f.stsc_n >= 1 && n >= 20) {
				f.first_chunk = be32(body + 8);
				f.per_chunk = be32(body + 12);
			}
		} else if (is(type, "stsz") && n >= 12) {
			f.stsz = true;
			f.size = be32(body + 4);
			f.count = be32(body + 8);
		} else if (is(type, "stco") && n >= 8) {
			f.stco = true;
			f.stco_n = be32(body + 4);
			if (f.stco_n >= 1 && n >= 12)
				f.chunk_off = be32(body + 8);
		} else if (is(type, "co64")) {
			f.co64 = true;
		}
		p = next;
	}
	return true;
}

// ---- build ----

struct Buf {
	std::vector<uint8_t> b;
	void u8(uint32_t v) { b.push_back((uint8_t)v); }
	void u16(uint32_t v) { u8(v >> 8); u8(v); }
	void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
	void tag(const char *t) { b.insert(b.end(), t, t + 4); }
	void zeros(size_t n) { b.insert(b.end(), n, 0); }
	void bytes(const std::vector<uint8_t> &v) { b.insert(b.end(), v.begin(), v.end()); }
	void str(const char *s, size_t n) { b.insert(b.end(), s, s + n); }
	size_t open(const char *t)
	{
		size_t at = b.size();
		u32(0);
		tag(t);
		return at;
	}
	size_t open_full(const char *t, uint32_t version, uint32_t flags)
	{
		size_t at = open(t);
		u32(version << 24 | flags);
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
	void matrix()
	{
		const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
		for (uint32_t v : m)
			u32(v);
	}
};

struct Layout {
	unsigned width, height;
	uint32_t timescale, delta, declared, size, dur_ms, dur_media;
};

void moov(Buf &f, const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps,
          const Layout &l, uint32_t chunk_offset)
{
	size_t moov = f.open("moov");

	size_t mvhd = f.open_full("mvhd", 0, 0);
	f.u32(0);
	f.u32(0);
	f.u32(1000);
	f.u32(l.dur_ms);
	f.u32(0x10000);
	f.u16(0x100);
	f.zeros(10);
	f.matrix();
	f.zeros(24);
	f.u32(2);
	f.close(mvhd);

	size_t trak = f.open("trak");
	size_t tkhd = f.open_full("tkhd", 0, 3);
	f.u32(0);
	f.u32(0);
	f.u32(1);
	f.u32(0);
	f.u32(l.dur_ms);
	f.zeros(8);
	f.u16(0);
	f.u16(0);
	f.u16(0);
	f.u16(0);
	f.matrix();
	f.u32(l.width << 16);
	f.u32(l.height << 16);
	f.close(tkhd);

	size_t mdia = f.open("mdia");
	size_t mdhd = f.open_full("mdhd", 0, 0);
	f.u32(0);
	f.u32(0);
	f.u32(l.timescale);
	f.u32(l.dur_media);
	f.u16(0x55c4);
	f.u16(0);
	f.close(mdhd);
	size_t hdlr = f.open_full("hdlr", 0, 0);
	f.u32(0);
	f.tag("vide");
	f.zeros(12);
	f.str("VideoHandler", 13);
	f.close(hdlr);

	size_t minf = f.open("minf");
	size_t vmhd = f.open_full("vmhd", 0, 1);
	f.zeros(8);
	f.close(vmhd);
	size_t dinf = f.open("dinf");
	size_t dref = f.open_full("dref", 0, 0);
	f.u32(1);
	size_t url = f.open_full("url ", 0, 1);
	f.close(url);
	f.close(dref);
	f.close(dinf);

	size_t stbl = f.open("stbl");
	size_t stsd = f.open_full("stsd", 0, 0);
	f.u32(1);
	size_t avc1 = f.open("avc1");
	f.zeros(6);
	f.u16(1);
	f.zeros(16);
	f.u16(l.width);
	f.u16(l.height);
	f.u32(0x480000);
	f.u32(0x480000);
	f.u32(0);
	f.u16(1);
	f.zeros(32);
	f.u16(0x18);
	f.u16(0xffff);
	size_t avcc = f.open("avcC");
	f.u8(1);
	f.u8(sps[1]);
	f.u8(sps[2]);
	f.u8(sps[3]);
	f.u8(0xff);
	f.u8(0xe1);
	f.u16((uint32_t)sps.size());
	f.bytes(sps);
	f.u8(1);
	f.u16((uint32_t)pps.size());
	f.bytes(pps);
	f.close(avcc);
	f.close(avc1);
	f.close(stsd);

	size_t stts = f.open_full("stts", 0, 0);
	f.u32(1);
	f.u32(l.declared);
	f.u32(l.delta);
	f.close(stts);
	size_t stsc = f.open_full("stsc", 0, 0);
	f.u32(1);
	f.u32(1);
	f.u32(l.declared);
	f.u32(1);
	f.close(stsc);
	size_t stsz = f.open_full("stsz", 0, 0);
	f.u32(l.size);
	f.u32(l.declared);
	f.close(stsz);
	size_t stco = f.open_full("stco", 0, 0);
	f.u32(1);
	f.u32(chunk_offset);
	f.close(stco);
	f.close(stbl);
	f.close(minf);
	f.close(mdia);
	f.close(trak);
	f.close(moov);
}

void put_filler(uint8_t *p, uint32_t pad)
{
	// length, then forbidden 0 / nal_ref_idc 0 / type 12, ff..ff, 80
	uint32_t len = pad - 4;
	p[0] = (uint8_t)(len >> 24);
	p[1] = (uint8_t)(len >> 16);
	p[2] = (uint8_t)(len >> 8);
	p[3] = (uint8_t)len;
	p[4] = h264::kFiller;
	std::memset(p + 5, 0xff, pad - 6);
	p[pad - 1] = 0x80;
}

} // namespace

std::string parse(const uint8_t *head, size_t len, uint64_t file_size, Info &out)
{
	out = Info();
	if (len < 8 || !is(head + 4, "ftyp"))
		return "no ftyp box at the start";

	// top level: ftyp, (free/skip ...), moov, mdat; stop after moov
	const uint8_t *p = head, *end = head + len, *moov_end = nullptr;
	Found f;
	while (p + 8 <= end && moov_end == nullptr) {
		uint64_t size = be32(p);
		if (size == 1 && p + 16 <= end)
			size = be64(p + 8);
		if (size < 8)
			return "bad top-level box size";
		if (is(p + 4, "moov")) {
			if (size > (uint64_t)(end - p))
				return "moov is cut off (read more of the file)";
			if (!walk(p, p + size, f))
				return "a box inside moov runs past its end";
			moov_end = p + size;
		}
		if (size > (uint64_t)(end - p))
			break;
		p += size;
	}
	if (moov_end == nullptr)
		return "no complete moov box";

	// mdat right after moov, with an 8-byte header: io-media's MP4 demuxer
	// walks the top-level boxes with 32-bit sizes only (no 64-bit
	// "largesize") and refuses a box that ends past the file
	// (docs/phone-mirroring.md 7.9)
	if (p != moov_end || p + 8 > end)
		return "no mdat header after moov (read more of the file)";
	if (!is(p + 4, "mdat"))
		return "moov must be followed by mdat";
	const uint32_t mdat_size = be32(p);
	const uint64_t mdat_off = (uint64_t)(p - head);
	if (mdat_size == 1)
		return "mdat has a 64-bit size: io-media reads 32-bit box sizes only";
	if (f.co64)
		return "co64: 64-bit chunk offsets not supported";
	if (!f.mdhd || !f.stts || !f.stsc || !f.stsz || !f.stco)
		return "missing mdhd, stts, stsc, stsz or stco";
	if (f.size == 0)
		return "stsz has per-sample sizes, not one constant size";
	if (f.stts_n != 1)
		return "stts needs exactly one entry (one frame duration)";
	if (f.stsc_n != 1 || f.first_chunk != 1 || f.per_chunk != f.count)
		return "stsc needs one entry: every frame in chunk 1";
	if (f.stco_n != 1)
		return "stco needs exactly one chunk";
	if (f.timescale == 0 || f.delta == 0 || f.count == 0)
		return "zero timescale, frame duration or frame count";
	if (f.chunk_off != mdat_off + 8)
		return "the frames must start right after mdat's 8-byte header";

	out.data_off = f.chunk_off;
	out.sample_size = f.size;
	out.declared = f.count;
	out.timescale = f.timescale;
	out.delta = f.delta;
	if (out.total() >= (uint64_t)1 << 32)
		return "the stream is 4 GB or more: io-media's box sizes are 32-bit";
	if (mdat_size != 0 && mdat_off + mdat_size != out.total())
		return "mdat must be size 0 or end where the declared frames end";
	if (file_size > out.data_off)
		out.present = (uint32_t)((file_size - out.data_off) / out.sample_size);
	if (out.present > out.declared)
		out.present = out.declared;
	if (out.present == 0)
		return "the file holds no whole frame";
	return "";
}

uint32_t max_frames(uint32_t sample_size, uint32_t hdr_max)
{
	if (sample_size == 0)
		return 0;
	uint64_t room = ((uint64_t)1 << 32) - 1 - hdr_max;
	return (uint32_t)(room / sample_size);
}

std::string build_header(const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps,
                         unsigned fps, uint32_t sample_size, uint32_t declared,
                         std::vector<uint8_t> &out, bool exact_mdat)
{
	out.clear();
	h264::Sps s;
	h264::Pps p;
	std::string err = h264::parse_sps(sps.data(), sps.size(), s);
	if (err.empty())
		err = h264::parse_pps(pps.data(), pps.size(), p);
	if (!err.empty())
		return err;
	if (fps == 0 || fps > 60)
		return "frame rate must be 1..60";
	if (declared == 0)
		return "no frames declared";
	if (sample_size < 1024)
		return "sample size under 1 KB";
	if (sps.size() > 0xffff || pps.size() > 0xffff)
		return "SPS/PPS too long";

	Layout l;
	l.width = s.width;
	l.height = s.height;
	l.timescale = fps * 512;               // 25 fps -> 12800, ffmpeg's choice
	l.delta = 512;
	l.declared = declared;
	l.size = sample_size;
	l.dur_ms = (uint32_t)((uint64_t)declared * 1000 / fps);
	l.dur_media = (uint32_t)((uint64_t)declared * l.delta);

	Buf f;
	size_t ftyp = f.open("ftyp");
	f.tag("isom");
	f.u32(0x200);
	f.str("isomiso2avc1mp41", 16);
	f.close(ftyp);
	size_t head = f.b.size();
	moov(f, sps, pps, l, 0);                // once to learn its size
	uint32_t chunk_offset = (uint32_t)(f.b.size() + 8);
	f.b.resize(head);
	moov(f, sps, pps, l, chunk_offset);
	if ((uint64_t)chunk_offset + (uint64_t)declared * sample_size >= (uint64_t)1 << 32)
		return "the declared stream is 4 GB or more; declare fewer frames";
	f.u32(exact_mdat ? 8 + declared * sample_size : 0);
	f.tag("mdat");
	out.swap(f.b);
	return "";
}

std::string make_sample_nals(const std::vector<std::pair<const uint8_t *, size_t>> &nals,
                             uint32_t sample_size, std::vector<uint8_t> &out)
{
	size_t used = 0;
	unsigned kept = 0;

	for (const auto &n : nals)
		if (n.second > 0 && (n.first[0] & 0x1f) != h264::kAud) {
			used += 4 + n.second;
			kept++;
		}
	if (kept == 0)
		return "no NAL units";
	if (used != sample_size && used + kFillerMin > sample_size)
		return "frame too big: " + std::to_string(used) + " bytes, sample size " +
		       std::to_string(sample_size);
	out.resize(sample_size);
	uint8_t *p = out.data();
	for (const auto &n : nals) {
		if (n.second == 0 || (n.first[0] & 0x1f) == h264::kAud)
			continue;
		p[0] = (uint8_t)(n.second >> 24);
		p[1] = (uint8_t)(n.second >> 16);
		p[2] = (uint8_t)(n.second >> 8);
		p[3] = (uint8_t)n.second;
		std::memcpy(p + 4, n.first, n.second);
		p += 4 + n.second;
	}
	if (used < sample_size)
		put_filler(p, (uint32_t)(sample_size - used));
	return "";
}

std::string make_sample(const uint8_t *annexb, size_t n, uint32_t sample_size,
                        std::vector<uint8_t> &out)
{
	std::vector<std::pair<const uint8_t *, size_t>> nals;
	for (const h264::Nal &nal : h264::split_annexb(annexb, n))
		nals.emplace_back(nal.p, nal.n);
	return make_sample_nals(nals, sample_size, out);
}

void filler_sample(uint32_t sample_size, std::vector<uint8_t> &out)
{
	out.resize(sample_size);
	put_filler(out.data(), sample_size);
}

} // namespace csmp4

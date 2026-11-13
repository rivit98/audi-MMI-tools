// csmp4.cpp - see csmp4.h.
#include "csmp4.h"
#include "types.h"

#include "h264.h"

#include <cstring>
#include <utility>

namespace csmp4 {

namespace {

u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

u64 be64(const u8 *p)
{
	return (u64)be32(p) << 32 | be32(p + 4);
}

bool is(const u8 *type, const char *name)
{
	return std::memcmp(type, name, 4) == 0;
}

// ---- parse ----

struct Found {
	bool mdhd = false, stts = false, stsc = false, stsz = false, stco = false, co64 = false;
	u32 timescale = 0, delta = 0, stts_n = 0, stsc_n = 0, per_chunk = 0, first_chunk = 0;
	u32 size = 0, count = 0, stco_n = 0;
	u64 chunk_off = 0;
};

// Walk the boxes in [p, end); descend into the containers on the path to
// the sample table. Returns false if a box runs past end.
bool walk(const u8 *p, const u8 *end, Found &f)
{
	while (p + 8 <= end) {
		u64 size = be32(p);
		const u8 *type = p + 4, *body = p + 8;

		if (size == 1) {
			if (p + 16 > end)
				return false;
			size = be64(p + 8);
			body = p + 16;
		} else if (size == 0) {
			size = (u64)(end - p);
		}
		if (size < (u64)(body - p) || size > (u64)(end - p))
			return false;
		const u8 *next = p + size;
		const usize n = (usize)(next - body);

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
	std::vector<u8> b;
	void put8(u32 v) { b.push_back((u8)v); }
	void put16(u32 v) { put8(v >> 8); put8(v); }
	void put32(u32 v) { put16(v >> 16); put16(v & 0xffff); }
	void tag(const char *t) { b.insert(b.end(), t, t + 4); }
	void zeros(usize n) { b.insert(b.end(), n, 0); }
	void bytes(const std::vector<u8> &v) { b.insert(b.end(), v.begin(), v.end()); }
	void str(const char *s, usize n) { b.insert(b.end(), s, s + n); }
	usize open(const char *t)
	{
		usize at = b.size();
		put32(0);
		tag(t);
		return at;
	}
	usize open_full(const char *t, u32 version, u32 flags)
	{
		usize at = open(t);
		put32(version << 24 | flags);
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
	void matrix()
	{
		const u32 m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
		for (u32 v : m)
			put32(v);
	}
};

struct Layout {
	unsigned width, height;
	u32 timescale, delta, declared, size, dur_ms, dur_media;
};

void moov(Buf &f, const std::vector<u8> &sps, const std::vector<u8> &pps,
          const Layout &l, u32 chunk_offset)
{
	usize moov = f.open("moov");

	usize mvhd = f.open_full("mvhd", 0, 0);
	f.put32(0);
	f.put32(0);
	f.put32(1000);
	f.put32(l.dur_ms);
	f.put32(0x10000);
	f.put16(0x100);
	f.zeros(10);
	f.matrix();
	f.zeros(24);
	f.put32(2);
	f.close(mvhd);

	usize trak = f.open("trak");
	usize tkhd = f.open_full("tkhd", 0, 3);
	f.put32(0);
	f.put32(0);
	f.put32(1);
	f.put32(0);
	f.put32(l.dur_ms);
	f.zeros(8);
	f.put16(0);
	f.put16(0);
	f.put16(0);
	f.put16(0);
	f.matrix();
	f.put32(l.width << 16);
	f.put32(l.height << 16);
	f.close(tkhd);

	usize mdia = f.open("mdia");
	usize mdhd = f.open_full("mdhd", 0, 0);
	f.put32(0);
	f.put32(0);
	f.put32(l.timescale);
	f.put32(l.dur_media);
	f.put16(0x55c4);
	f.put16(0);
	f.close(mdhd);
	usize hdlr = f.open_full("hdlr", 0, 0);
	f.put32(0);
	f.tag("vide");
	f.zeros(12);
	f.str("VideoHandler", 13);
	f.close(hdlr);

	usize minf = f.open("minf");
	usize vmhd = f.open_full("vmhd", 0, 1);
	f.zeros(8);
	f.close(vmhd);
	usize dinf = f.open("dinf");
	usize dref = f.open_full("dref", 0, 0);
	f.put32(1);
	usize url = f.open_full("url ", 0, 1);
	f.close(url);
	f.close(dref);
	f.close(dinf);

	usize stbl = f.open("stbl");
	usize stsd = f.open_full("stsd", 0, 0);
	f.put32(1);
	usize avc1 = f.open("avc1");
	f.zeros(6);
	f.put16(1);
	f.zeros(16);
	f.put16(l.width);
	f.put16(l.height);
	f.put32(0x480000);
	f.put32(0x480000);
	f.put32(0);
	f.put16(1);
	f.zeros(32);
	f.put16(0x18);
	f.put16(0xffff);
	usize avcc = f.open("avcC");
	f.put8(1);
	f.put8(sps[1]);
	f.put8(sps[2]);
	f.put8(sps[3]);
	f.put8(0xff);
	f.put8(0xe1);
	f.put16((u32)sps.size());
	f.bytes(sps);
	f.put8(1);
	f.put16((u32)pps.size());
	f.bytes(pps);
	f.close(avcc);
	f.close(avc1);
	f.close(stsd);

	usize stts = f.open_full("stts", 0, 0);
	f.put32(1);
	f.put32(l.declared);
	f.put32(l.delta);
	f.close(stts);
	usize stsc = f.open_full("stsc", 0, 0);
	f.put32(1);
	f.put32(1);
	f.put32(l.declared);
	f.put32(1);
	f.close(stsc);
	usize stsz = f.open_full("stsz", 0, 0);
	f.put32(l.size);
	f.put32(l.declared);
	f.close(stsz);
	usize stco = f.open_full("stco", 0, 0);
	f.put32(1);
	f.put32(chunk_offset);
	f.close(stco);
	f.close(stbl);
	f.close(minf);
	f.close(mdia);
	f.close(trak);
	f.close(moov);
}

void put_filler(u8 *p, u32 pad)
{
	// length, then forbidden 0 / nal_ref_idc 0 / type 12, ff..ff, 80
	u32 len = pad - 4;
	p[0] = (u8)(len >> 24);
	p[1] = (u8)(len >> 16);
	p[2] = (u8)(len >> 8);
	p[3] = (u8)len;
	p[4] = h264::kFiller;
	std::memset(p + 5, 0xff, pad - 6);
	p[pad - 1] = 0x80;
}

} // namespace

std::string parse(const u8 *head, usize len, u64 file_size, Info &out)
{
	out = Info();
	if (len < 8 || !is(head + 4, "ftyp"))
		return "no ftyp box at the start";

	// top level: ftyp, (free/skip ...), moov, mdat; stop after moov
	const u8 *p = head, *end = head + len, *moov_end = nullptr;
	Found f;
	while (p + 8 <= end && moov_end == nullptr) {
		u64 size = be32(p);
		if (size == 1 && p + 16 <= end)
			size = be64(p + 8);
		if (size < 8)
			return "bad top-level box size";
		if (is(p + 4, "moov")) {
			if (size > (u64)(end - p))
				return "moov is cut off (read more of the file)";
			if (!walk(p, p + size, f))
				return "a box inside moov runs past its end";
			moov_end = p + size;
		}
		if (size > (u64)(end - p))
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
	const u32 mdat_size = be32(p);
	const u64 mdat_off = (u64)(p - head);
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
	if (out.total() >= (u64)1 << 32)
		return "the stream is 4 GB or more: io-media's box sizes are 32-bit";
	if (mdat_size != 0 && mdat_off + mdat_size != out.total())
		return "mdat must be size 0 or end where the declared frames end";
	if (file_size > out.data_off)
		out.present = (u32)((file_size - out.data_off) / out.sample_size);
	if (out.present > out.declared)
		out.present = out.declared;
	if (out.present == 0)
		return "the file holds no whole frame";
	return "";
}

u32 max_frames(u32 sample_size, u32 hdr_max)
{
	if (sample_size == 0)
		return 0;
	u64 room = ((u64)1 << 32) - 1 - hdr_max;
	return (u32)(room / sample_size);
}

std::string build_header(const std::vector<u8> &sps, const std::vector<u8> &pps,
                         unsigned fps, u32 sample_size, u32 declared,
                         std::vector<u8> &out, bool exact_mdat)
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
	l.dur_ms = (u32)((u64)declared * 1000 / fps);
	l.dur_media = (u32)((u64)declared * l.delta);

	Buf f;
	usize ftyp = f.open("ftyp");
	f.tag("isom");
	f.put32(0x200);
	f.str("isomiso2avc1mp41", 16);
	f.close(ftyp);
	usize head = f.b.size();
	moov(f, sps, pps, l, 0);                // once to learn its size
	u32 chunk_offset = (u32)(f.b.size() + 8);
	f.b.resize(head);
	moov(f, sps, pps, l, chunk_offset);
	if ((u64)chunk_offset + (u64)declared * sample_size >= (u64)1 << 32)
		return "the declared stream is 4 GB or more; declare fewer frames";
	f.put32(exact_mdat ? 8 + declared * sample_size : 0);
	f.tag("mdat");
	out.swap(f.b);
	return "";
}

std::string make_sample_nals(const std::vector<std::pair<const u8 *, usize>> &nals,
                             u32 sample_size, std::vector<u8> &out)
{
	usize used = 0;
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
	u8 *p = out.data();
	for (const auto &n : nals) {
		if (n.second == 0 || (n.first[0] & 0x1f) == h264::kAud)
			continue;
		p[0] = (u8)(n.second >> 24);
		p[1] = (u8)(n.second >> 16);
		p[2] = (u8)(n.second >> 8);
		p[3] = (u8)n.second;
		std::memcpy(p + 4, n.first, n.second);
		p += 4 + n.second;
	}
	if (used < sample_size)
		put_filler(p, (u32)(sample_size - used));
	return "";
}

std::string make_sample(const u8 *annexb, usize n, u32 sample_size,
                        std::vector<u8> &out)
{
	std::vector<std::pair<const u8 *, usize>> nals;
	for (const h264::Nal &nal : h264::split_annexb(annexb, n))
		nals.emplace_back(nal.p, nal.n);
	return make_sample_nals(nals, sample_size, out);
}

void filler_sample(u32 sample_size, std::vector<u8> &out)
{
	out.resize(sample_size);
	put_filler(out.data(), sample_size);
}

} // namespace csmp4

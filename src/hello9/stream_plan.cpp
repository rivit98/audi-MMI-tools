// stream_plan.cpp - see stream_plan.h.
#include "stream_plan.h"

#include <cstring>
#include <utility>

namespace plan {

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

} // namespace

std::string parse(const uint8_t *head, size_t len, uint64_t file_size, CsMp4 &out)
{
	out = CsMp4();
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

Schedule::Schedule(double fps, std::vector<Stall> stalls) : fps_(fps), stalls_(std::move(stalls)) {}

uint64_t Schedule::stall_ms_before(uint64_t frame) const
{
	uint64_t ms = 0;

	for (const Stall &s : stalls_)
		if (s.at_frame <= frame)
			ms += s.ms;
	return ms;
}

uint64_t Schedule::release_ms(uint64_t frame) const
{
	return (uint64_t)((double)frame * 1000.0 / fps_ + 1e-6) + stall_ms_before(frame);
}

uint64_t Schedule::released(uint64_t elapsed_ms) const
{
	// release_ms is monotonic: the first frame not yet released, by bisection
	uint64_t lo = 0, hi = (uint64_t)((double)elapsed_ms * fps_ / 1000.0) + 2;

	while (lo < hi) {
		uint64_t mid = lo + (hi - lo) / 2;
		if (release_ms(mid) <= elapsed_ms)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

Span map(const CsMp4 &m, uint64_t off)
{
	if (off < m.data_off)
		return {off, m.data_off - off};
	uint64_t k = (off - m.data_off) / m.sample_size;
	uint64_t j = (off - m.data_off) % m.sample_size;
	return {m.data_off + (k % m.present) * m.sample_size + j, m.sample_size - j};
}

uint64_t frames_needed(const CsMp4 &m, uint64_t end)
{
	if (end <= m.data_off)
		return 0;
	return (end - m.data_off + m.sample_size - 1) / m.sample_size;
}

} // namespace plan

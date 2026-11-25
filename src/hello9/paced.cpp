// paced.cpp - see paced.h.
#include "paced.h"
#include "types.h"

#include <cstring>

namespace paced {

namespace {

u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

// The first full box of this type inside [p, end) at any depth below the
// containers on the path moov / trak / mdia / minf / stbl; its payload
// (after the 4-byte version + flags) and size. nullptr if none.
const u8 *find_box(const u8 *p, const u8 *end, const char *type, u32 *payload)
{
	static const char *const kContainers[] = {"moov", "trak", "mdia", "minf", "stbl"};
	while (end - p >= 8) {
		u32 size = be32(p);
		if (size < 8 || size > (u64)(end - p))
			return nullptr;
		if (std::memcmp(p + 4, type, 4) == 0) {
			if (size < 12)
				return nullptr;
			*payload = size - 12;
			return p + 12;
		}
		for (const char *c : kContainers)
			if (std::memcmp(p + 4, c, 4) == 0) {
				if (const u8 *r = find_box(p + 8, p + size, type, payload))
					return r;
				break;
			}
		p += size;
	}
	return nullptr;
}

} // namespace

std::string parse_layout(const std::vector<u8> &file, Layout &out)
{
	const u8 *b = file.data(), *e = b + file.size();
	u32 n = 0;
	const u8 *stco = find_box(b, e, "stco", &n);
	if (stco == nullptr || n < 4)
		return "no stco";
	u32 chunks = be32(stco);
	if (chunks < 2 || (u64)n < 4 + 4ull * chunks)
		return "stco: fewer than 2 chunks";
	const u8 *stsc = find_box(b, e, "stsc", &n);
	if (stsc == nullptr || n < 16 || be32(stsc) != 1)
		return "stsc: not one entry";
	out.data_off = be32(stco + 4);
	u32 second = be32(stco + 8);
	if (second <= out.data_off)
		return "stco: chunks out of order";
	out.chunk = second - (u32)out.data_off;
	out.chunks = chunks;
	out.video_per_chunk = be32(stsc + 8);
	for (u32 k = 0; k < chunks; k++)
		if (be32(stco + 4 + 4 * k) != out.data_off + (u64)k * out.chunk)
			return "stco: chunks not evenly spaced";
	if (out.video_per_chunk == 0)
		return "stsc: no frames per chunk";
	if (out.data_off + (u64)chunks * out.chunk != file.size())
		return "the file is not data_off + chunks * chunk bytes";
	return "";
}

std::string PacedFile::configure(std::vector<u8> file, unsigned fps)
{
	if (fps == 0)
		return "fps 0";
	std::string err = parse_layout(file, lay_);
	if (!err.empty())
		return err;
	data_ = std::move(file);
	slot_ms_ = lay_.video_per_chunk * 1000 / fps;
	if (slot_ms_ == 0)
		return "chunk shorter than 1 ms";
	started_ = finished_ = false;
	released_ = 0;
	return "";
}

void PacedFile::start(u64 now_ms)
{
	t0_ = now_ms;
	started_ = true;
}

unsigned PacedFile::slots_per_s() const
{
	unsigned s = slot_ms_ ? 1000 / slot_ms_ : 1;
	return s ? s : 1;
}

unsigned PacedFile::fill(u64 now_ms)
{
	if (!started_ || now_ms < t0_)
		return 0;
	u64 due = (now_ms - t0_) / slot_ms_;
	if (due > lay_.chunks)
		due = lay_.chunks;
	u64 was = released_;
	if (due <= was)
		return 0;
	released_ = (u32)due;
	return (unsigned)(due - was);
}

u64 PacedFile::available() const
{
	if (finished_)
		return data_.size();
	return lay_.data_off + (u64)released_ * lay_.chunk;
}

usize PacedFile::read(u64 off, u8 *dst, usize n, ReadInfo *info)
{
	if (info != nullptr)
		*info = ReadInfo();
	if (off >= data_.size())
		return 0;
	if (off + n > data_.size())
		n = (usize)(data_.size() - off);
	u64 avail = available();
	if (info != nullptr && off + n > avail && lay_.chunk)
		info->ahead = (unsigned)((off + n - (off > avail ? off : avail) + lay_.chunk - 1) /
		                         lay_.chunk);
	std::memcpy(dst, data_.data() + off, n);
	return n;
}

} // namespace paced

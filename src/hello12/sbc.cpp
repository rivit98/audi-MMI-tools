// sbc.cpp - see sbc.h.
#include "sbc.h"
#include "types.h"

#include <cstdio>

namespace sbc {

usize frame_len(u8 mode, u8 subbands, u8 blocks, u8 bitpool)
{
	u32 ch = mode == kMono ? 1 : 2;
	u32 bits;

	switch (mode) {
	case kMono:
	case kDual:
		bits = (u32)blocks * ch * bitpool;
		break;
	case kStereo:
		bits = (u32)blocks * bitpool;
		break;
	default:
		bits = (u32)subbands + (u32)blocks * bitpool;
		break;
	}
	return 4 + (4u * subbands * ch) / 8 + (bits + 7) / 8;
}

bool parse(const u8 *p, usize n, Header &h)
{
	static const u32 kRates[4] = {16000, 32000, 44100, 48000};

	if (n < 4 || p[0] != 0x9C)
		return false;
	h.rate = kRates[(p[1] >> 6) & 3];
	h.blocks = (u8)(4 * (((p[1] >> 4) & 3) + 1));
	h.mode = (u8)((p[1] >> 2) & 3);
	h.loudness = (u8)(((p[1] >> 1) & 1) == 0);
	h.subbands = (p[1] & 1) ? 8 : 4;
	h.bitpool = p[2];
	h.channels = h.mode == kMono ? 1 : 2;
	h.samples = (u32)h.blocks * h.subbands;
	// A2DP: bitpool 2..250, at most 16 * subbands per channel (mono/dual)
	// or 32 * subbands (stereo/joint)
	u32 maxpool = (h.mode == kMono || h.mode == kDual) ? 16u * h.subbands : 32u * h.subbands;
	if (h.bitpool < 2 || h.bitpool > maxpool)
		return false;
	h.frame_len = frame_len(h.mode, h.subbands, h.blocks, h.bitpool);
	return true;
}

std::string index(const u8 *data, usize n, std::vector<u32> &offs, Header *first)
{
	Header h0, h;
	usize off = 0;
	char why[120];

	offs.clear();
	while (off < n) {
		if (!parse(data + off, n - off, h)) {
			std::snprintf(why, sizeof(why), "no SBC frame header at byte %lu",
			              (unsigned long)off);
			return why;
		}
		if (offs.empty()) {
			h0 = h;
		} else if (h.rate != h0.rate || h.frame_len != h0.frame_len || h.mode != h0.mode) {
			std::snprintf(why, sizeof(why), "frame %lu at byte %lu changes format",
			              (unsigned long)offs.size(), (unsigned long)off);
			return why;
		}
		if (off + h.frame_len > n) {
			std::snprintf(why, sizeof(why), "frame %lu at byte %lu is cut off",
			              (unsigned long)offs.size(), (unsigned long)off);
			return why;
		}
		offs.push_back((u32)off);
		off += h.frame_len;
	}
	if (offs.empty())
		return "empty";
	if (first != nullptr)
		*first = h0;
	return "";
}

usize released(usize count, u32 samples, u32 rate, usize prebuffer, u64 elapsed_us)
{
	if (samples == 0 || rate == 0)
		return count;
	// frames whose time (elapsed * rate / samples) has come, + prebuffer
	u64 due = elapsed_us * rate / ((u64)samples * 1000000u) + 1 + prebuffer;
	return due < count ? (usize)due : count;
}

bool answer(u64 off, u32 nbytes, u64 avail, u64 total, bool complete, bool partial, u32 *n)
{
	if (off >= total) {
		*n = 0;
		return true;
	}
	u64 end = off + nbytes;
	if (end > total)
		end = total;
	if (end <= avail || complete) {
		u64 have = avail < end ? avail : end;
		*n = have > off ? (u32)(have - off) : 0;
		return true;
	}
	if (partial && avail > off) {
		*n = (u32)(avail - off);
		return true;
	}
	return false;
}

} // namespace sbc

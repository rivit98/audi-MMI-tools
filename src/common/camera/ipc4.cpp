// ipc4.cpp - see ipc4.h.
#include "ipc4.h"

#include <cstdio>

namespace ipc4 {

namespace {

constexpr usize kHead = 4;              // counter, type, len u16
constexpr usize kTail = 2;              // FA DE
constexpr usize kMaxLen = 4096;         // longer: not a frame (the biggest seen is ~30)
constexpr usize kMostHead = 10;

u16 le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

} // namespace

isize Splitter::frame_at(usize at) const
{
	const u8 *h = buf_.data() + at;
	usize len = le16(h + 2);
	if (len == 0 || len > kMaxLen)
		return 0;
	usize total = kHead + (len - 1) + kTail;
	if (buf_.size() - at < total)
		return -1;
	if (h[total - 2] != 0xFA || h[total - 1] != 0xDE)
		return 0;
	return (isize)total;
}

void Splitter::feed(const u8 *p, usize n, std::vector<Telegram> &out)
{
	if (!init_) {
		for (int &c : last_)
			c = -1;
		init_ = true;
	}
	buf_.insert(buf_.end(), p, p + n);
	usize at = 0;
	while (buf_.size() - at >= kHead) {
		isize total = frame_at(at);
		if (total == 0) {
			at++;
			skipped_++;
			continue;
		}
		if (total < 0) {
			// Incomplete: real, or junk that looks like a long header. A
			// complete frame further on means junk.
			usize j = at + 1;
			while (j + kHead <= buf_.size() && frame_at(j) <= 0)
				j++;
			if (j + kHead > buf_.size())
				break;                  // wait for the rest
			skipped_ += (u32)(j - at);
			at = j;
			continue;
		}
		const u8 *h = buf_.data() + at;
		Telegram t;
		t.counter = h[0];
		t.type = h[1];
		t.body.assign(h + kHead, h + (usize)total - kTail);
		int &last = last_[t.type];
		if (last >= 0 && t.counter != (u8)(last + 1)) {
			gaps_++;
			missing_ += (u8)(t.counter - (u8)(last + 1));
		}
		last = t.counter;
		telegrams_++;
		out.push_back(std::move(t));
		at += (usize)total;
	}
	buf_.erase(buf_.begin(), buf_.begin() + (isize)at);
	// A stream that never frames: keep the buffer bounded.
	if (buf_.size() > 2 * kMaxLen) {
		skipped_ += (u32)(buf_.size() - kHead);
		buf_.erase(buf_.begin(), buf_.end() - kHead);
	}
}

bool parse_most(const std::vector<u8> &b, Most &out)
{
	if (b.size() < kMostHead || b[0] != 0x00 || b[9] != 0xFF)
		return false;
	usize plen = le16(&b[7]);
	if (kMostHead + plen != b.size())
		return false;
	out.fblock = b[1];
	out.inst = b[2];
	out.fct = (u16)((b[3] << 8) | b[4]);
	out.op = le16(&b[5]);
	out.payload.assign(b.begin() + kMostHead, b.end());
	return true;
}

std::string most_key(const Most &m)
{
	char s[32];
	std::snprintf(s, sizeof(s), "%02X%02X.%04X/%X", m.fblock, m.inst, m.fct, m.op);
	return s;
}

bool is_bap(const Most &m)
{
	return m.fblock == kBapFblock && m.fct == kBapFct;
}

bool parse_bap(const std::vector<u8> &s, std::vector<BapFn> &out)
{
	out.clear();
	if (s.size() < 4 || s[0] == 0)
		return false;
	usize at = 1;
	while (at < s.size()) {
		if (s.size() - at < 3)
			return false;
		BapFn f;
		f.lsg = s[0];
		f.fid = s[at];
		usize len = le16(&s[at + 1]);
		at += 3;
		if (len == 0 || s.size() - at < len)
			return false;
		f.payload.assign(s.begin() + (isize)at, s.begin() + (isize)(at + len));
		at += len;
		out.push_back(std::move(f));
	}
	return true;
}

std::string bap_key(const BapFn &f)
{
	char s[24];
	std::snprintf(s, sizeof(s), "BAP %02X.%02X", f.lsg, f.fid);
	return s;
}

std::string hex(const u8 *p, usize n, usize max)
{
	std::string s;
	char b[4];
	for (usize i = 0; i < n && i < max; i++) {
		std::snprintf(b, sizeof(b), i ? " %02x" : "%02x", p[i]);
		s += b;
	}
	if (n > max)
		s += " ..";
	return s;
}

std::string hex(const std::vector<u8> &v, usize max)
{
	return hex(v.data(), v.size(), max);
}

} // namespace ipc4

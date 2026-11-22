// sources.cpp - see sources.h.
#include "sources.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace ch5 {

namespace {

const char *name(u8 tag)
{
	switch (tag) {
	case 0x01: return "GYRO";
	case 0x02: return "RIZ";
	case 0x03: return "REV";
	case 0x04: return "STR_ANGLE";
	case 0x05: return "ExternalGYRO";
	case 0x06: return "BrakeLightESP";
	case 0x07: return "TractionBrake";
	case 0x08: return "OutsideTemp";
	case 0x09: return "RainSens";
	case 0x0a: return "Wiper";
	case 0x0b: return "CURVETRONIC";
	case 0x0c: return "RevGear";
	case 0x0d: return "SpeedoPulse";
	case 0x0e: return "GearPosition";
	case 0x0f: return "TEMPERATURE";
	case 0x10: return "3DAcceleration";
	case 0x11: return "3DAccelerationHr";
	case 0xf0: return "TIMEDATE";
	case 0xff: return "PROTOCOL";
	default: return "?";
	}
}

} // namespace

std::vector<Record> parse(const u8 *p, usize n, usize *skipped)
{
	std::vector<Record> out;
	usize i = 0;
	while (i + 4 <= n) {
		usize len = (usize)p[i + 2] | ((usize)p[i + 3] << 8);
		if (i + 4 + len + 2 > n)
			break;
		Record r;
		r.tag = p[i];
		r.subtag = p[i + 1];
		r.payload.assign(p + i + 4, p + i + 4 + len);
		r.ts = (u16)(p[i + 4 + len] | (p[i + 5 + len] << 8));
		out.push_back(r);
		i += 4 + len + 2;
	}
	if (skipped != nullptr)
		*skipped = n - i;
	return out;
}

bool noisy(u8 tag)
{
	return tag == 0x01 || tag == 0x05 || tag == 0x10 || tag == 0x11 || tag == 0xf0;
}

std::string key(const Record &r)
{
	char s[48];
	std::snprintf(s, sizeof(s), "ch5 %02x.%02x %s", r.tag, r.subtag, name(r.tag));
	return s;
}

} // namespace ch5

namespace slogtext {

std::vector<std::string> lines(const int *ev, usize n_ints)
{
	std::vector<std::string> out;
	usize i = 0;
	while (i + 3 <= n_ints) {
		usize count = ((unsigned)ev[i] >> 16) & 0xff;
		bool text = (ev[i] & 0x8) != 0;
		if (i + 3 + count > n_ints)
			break;
		if (text) {
			const char *t = reinterpret_cast<const char *>(ev + i + 3);
			usize max = count * sizeof(int);
			const void *end = std::memchr(t, 0, max);
			out.emplace_back(t, end ? (usize)(static_cast<const char *>(end) - t) : max);
		}
		i += 3 + count;
	}
	return out;
}

bool interesting(const std::string &line)
{
	static const char *const words[] = {"bright", "dimm", "daynight", "day/night", "nightview",
	                                    "dayview", "nightmode", "externlight", "illum",
	                                    "lightsens", "kl58", "klemme58", "terminal58"};
	std::string low;
	for (char c : line)
		low += (char)std::tolower((u8)c);
	for (const char *w : words)
		if (low.find(w) != std::string::npos)
			return true;
	return false;
}

std::string key(const std::string &line)
{
	std::string out;
	bool in_num = false;
	for (char c : line) {
		bool d = std::isdigit((u8)c) != 0;
		if (d && !in_num)
			out += '#';
		else if (!d)
			out += c;
		in_num = d;
	}
	return out;
}

std::string numbers(const std::string &line)
{
	std::string out;
	bool in_num = false;
	for (char c : line) {
		bool d = std::isdigit((u8)c) != 0;
		if (d && !in_num && !out.empty())
			out += ' ';
		if (d)
			out += c;
		in_num = d;
	}
	return out;
}

} // namespace slogtext

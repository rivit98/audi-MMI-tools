/*
 * mapinfo.cpp - see mapinfo.h.
 */
#include "mapinfo.h"

#include <cstdio>

namespace mapinfo {

namespace {

struct Bit {
	u32 bit;
	const char *name;
};

// Everything but the share type and the protection, low bit first.
const Bit kBits[] = {
	{0x00000010, "FIXED"},    {0x00000020, "ELF"},      {0x00000040, "NOSYNCFILE"},
	{0x00000080, "LAZY"},     {0x00001000, "STACK"},    {0x00002000, "BELOW"},
	{0x00004000, "NOINIT"},   {0x00010000, "PHYS"},     {0x00020000, "NOX64K"},
	{0x00040000, "BELOW16M"}, {0x00080000, "ANON"},     {0x00100000, "PG_MODIFIED"},
	{0x00200000, "PG_REFERENCED"}, {0x01000000, "SYSRAM"},
};

constexpr u32 kProtMask = 0x00000f00;

} // namespace

std::string flags_str(u32 flags)
{
	std::string s;
	switch (flags & kTypeMask) {
	case kShared: s = "SHARED"; break;
	case kPrivate: s = "PRIVATE"; break;
	default: {
		char b[24];
		std::snprintf(b, sizeof(b), "type%u", (unsigned)(flags & kTypeMask));
		s = b;
	}
	}
	u32 rest = flags & ~(kTypeMask | kProtMask);
	for (const Bit &b : kBits)
		if (rest & b.bit) {
			s += " ";
			s += b.name;
			rest &= ~b.bit;
		}
	if (rest != 0) {
		char b[24];
		std::snprintf(b, sizeof(b), " 0x%08x", (unsigned)rest);
		s += b;
	}
	s += " ";
	s += (flags & kProtRead) ? "r" : "-";
	s += (flags & kProtWrite) ? "w" : "-";
	s += (flags & kProtExec) ? "x" : "-";
	return s;
}

std::string write_verdict(u32 flags)
{
	if (flags & kProtWrite)
		return "writable: a /proc write lands, in this process only if PRIVATE";
	if ((flags & kTypeMask) == kShared)
		return "read-only SHARED: a /proc write faults (ESRVRFAULT); the page is "
		       "the one every process running this file uses";
	return "read-only: a /proc write faults (ESRVRFAULT); procnto never copies the page";
}

const Seg *find(const std::vector<Seg> &segs, u64 addr)
{
	for (const Seg &s : segs)
		if (addr >= s.vaddr && addr - s.vaddr < s.size)
			return &s;
	return nullptr;
}

} // namespace mapinfo

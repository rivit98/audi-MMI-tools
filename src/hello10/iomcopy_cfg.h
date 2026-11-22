// iomcopy_cfg.h - the pure parts of hello10's io-media copy (iomcopy.h),
// host-tested (tests/test_hello10_iomcopy.cpp).
#ifndef HELLO10_IOMCOPY_CFG_H
#define HELLO10_IOMCOPY_CFG_H

#include "types.h"

#include <set>
#include <string>
#include <vector>

namespace iomcopy {

// io-media-nvidia's capture width for the DSP input: three 16-bit words,
// 720 little endian (`d0 02`), at these file offsets (vaddr 0x0818a190,
// 0x0818a26e, 0x0818ad20 minus the text segment's 0x08040000;
// docs/phone-mirroring.md 7.14).
constexpr usize kWidthOffsets[3] = {0x14a190, 0x14a26e, 0x14ad20};

// Sets the capture width words from `from` to `to`. Each must read `from`
// first; nothing is changed unless all three do. Returns "" or the error.
inline std::string patch_width(std::vector<u8> &bin, u16 from, u16 to)
{
	for (usize off : kWidthOffsets) {
		if (off + 2 > bin.size())
			return "file too short for offset " + std::to_string(off);
		u16 v = (u16)(bin[off] | bin[off + 1] << 8);
		if (v != from)
			return "offset " + std::to_string(off) + " holds " + std::to_string(v) + ", not " +
			       std::to_string(from);
	}
	for (usize off : kWidthOffsets) {
		bin[off] = (u8)(to & 0xff);
		bin[off + 1] = (u8)(to >> 8);
	}
	return "";
}

// iom.cfg with `mountpoint = "<mount>"` in its global-options block: the
// path io-media attaches instead of /dev/io-media (io-media-nvidia's option
// parsing, P1001 0x080892b0: the default "/dev/io-media", then the
// "mountpoint" value of global-options). An existing mountpoint line is
// replaced. Returns "" with *out set, or the error.
inline std::string with_mountpoint(const std::string &cfg, const std::string &mount,
                                   std::string *out)
{
	usize g = cfg.find("global-options");
	// skip commented-out mentions: the word must start its line's text
	while (g != std::string::npos) {
		usize line = cfg.rfind('\n', g);
		line = line == std::string::npos ? 0 : line + 1;
		if (cfg.find_first_not_of(" \t", line) == g)
			break;
		g = cfg.find("global-options", g + 1);
	}
	if (g == std::string::npos)
		return "no global-options block";
	usize open = cfg.find('{', g);
	if (open == std::string::npos)
		return "global-options has no {";
	usize close = cfg.find('}', open);
	if (close == std::string::npos)
		return "global-options has no }";
	if (cfg.find('{', open + 1) < close)
		return "global-options has a nested block";

	const std::string line = "mountpoint = \"" + mount + "\"";
	std::string r = cfg;
	usize m = r.find("mountpoint", open);
	while (m != std::string::npos && m < close) {
		usize ls = r.rfind('\n', m);
		ls = ls == std::string::npos ? 0 : ls + 1;
		usize first = r.find_first_not_of(" \t", ls);
		if (first == m) {   // a real line, not a comment
			usize le = r.find('\n', m);
			r.replace(m, (le == std::string::npos ? r.size() : le) - m, line);
			*out = r;
			return "";
		}
		m = r.find("mountpoint", m + 1);
	}
	r.insert(open + 1, "\n\t\t" + line);
	*out = r;
	return "";
}

// The lines of text, without their '\n' (no <sstream>: it pulls in
// libstdc++'s locale code, which the SDK can't link).
inline std::vector<std::string> lines_of(const std::string &text)
{
	std::vector<std::string> out;
	usize i = 0;
	while (i < text.size()) {
		usize j = text.find('\n', i);
		if (j == std::string::npos)
			j = text.size();
		out.push_back(text.substr(i, j - i));
		i = j + 1;
	}
	return out;
}

// The lines of `after` that are not in `before` (order of `after`): what a
// listing gained while the copy started.
inline std::vector<std::string> added_lines(const std::string &before, const std::string &after)
{
	std::vector<std::string> b = lines_of(before);
	std::set<std::string> had(b.begin(), b.end());
	std::vector<std::string> out;
	for (const std::string &l : lines_of(after))
		if (!l.empty() && had.count(l) == 0)
			out.push_back(l);
	return out;
}

} // namespace iomcopy

#endif

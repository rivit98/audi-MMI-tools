// test_hello10_iomcopy.cpp - the pure parts of hello10's io-media copy
// (src/hello10/iomcopy_cfg.h): the config's mountpoint, the width patch,
// the listing diff.
#include "types.h"
#include "check.h"
#include "iomcopy_cfg.h"

#include <string>
#include <vector>

namespace {

// The head of the unit's /etc/iom.cfg (efs-system), plus a comment that
// names global-options.
const char kCfg[] = "# io-media config; see global-options below\n"
                    "io-media-config {\n"
                    "        global-options {\n"
                    "                num-threads = 4\n"
                    "                audio-hi-prio=22\n"
                    "        }\n"
                    "        module-options { \n"
                    "                module = \"mediafs_2wire\" \n"
                    "        } \n"
                    "}\n";

void test_mountpoint()
{
	std::string out;
	CHECK(iomcopy::with_mountpoint(kCfg, "/dev/x-iom", &out).empty());
	usize g = out.find("        global-options {");
	usize m = out.find("mountpoint = \"/dev/x-iom\"");
	usize n = out.find("num-threads");
	CHECK(g != std::string::npos && m != std::string::npos);
	CHECK(g < m && m < n);                        // inside the block, first
	CHECK(out.find("mountpoint", m + 1) == std::string::npos);   // once
	// the rest is unchanged
	std::string back = out;
	back.erase(back.find("\n\t\tmountpoint"), std::string("\n\t\tmountpoint = \"/dev/x-iom\"").size());
	CHECK(back == kCfg);

	// a second run replaces the line instead of adding one
	std::string again;
	CHECK(iomcopy::with_mountpoint(out, "/dev/y", &again).empty());
	CHECK(again.find("/dev/x-iom") == std::string::npos);
	CHECK(again.find("mountpoint = \"/dev/y\"") != std::string::npos);
	CHECK(again.find("mountpoint", again.find("mountpoint") + 1) == std::string::npos);

	// a commented mountpoint is left alone and a real one added
	std::string commented = "global-options {\n  # mountpoint = \"/old\"\n  a = 1\n}\n", c;
	CHECK(iomcopy::with_mountpoint(commented, "/dev/z", &c).empty());
	CHECK(c.find("# mountpoint = \"/old\"") != std::string::npos);
	CHECK(c.find("mountpoint = \"/dev/z\"") < c.find("# mountpoint"));

	std::string bad;
	CHECK(!iomcopy::with_mountpoint("io-media-config { }\n", "/d", &bad).empty());
	CHECK(!iomcopy::with_mountpoint("# global-options {\n", "/d", &bad).empty());
	CHECK(!iomcopy::with_mountpoint("global-options { a { } }\n", "/d", &bad).empty());
}

void test_patch()
{
	std::vector<u8> bin(0x14b000, 0x11);
	for (usize off : iomcopy::kWidthOffsets) {
		bin[off] = 0xd0;
		bin[off + 1] = 0x02;
	}
	std::vector<u8> want = bin;
	for (usize off : iomcopy::kWidthOffsets) {
		want[off] = 0x20;
		want[off + 1] = 0x03;
	}
	CHECK(iomcopy::patch_width(bin, 720, 800).empty());
	CHECK(bin == want);
	// already patched: refused, nothing changed
	CHECK(!iomcopy::patch_width(bin, 720, 800).empty());
	CHECK(bin == want);
	// one word off: nothing changed at all
	std::vector<u8> mixed(0x14b000, 0);
	mixed[iomcopy::kWidthOffsets[0]] = 0xd0;
	mixed[iomcopy::kWidthOffsets[0] + 1] = 0x02;
	std::vector<u8> before = mixed;
	CHECK(!iomcopy::patch_width(mixed, 720, 800).empty());
	CHECK(mixed == before);
	std::vector<u8> small(16, 0);
	CHECK(!iomcopy::patch_width(small, 720, 800).empty());
}

void test_added()
{
	std::vector<std::string> a = iomcopy::added_lines("== /dev\nio-media\nslog\n",
	                                                   "== /dev\nhello10-iom\nio-media\nslog\n\n");
	CHECK_EQ(a.size(), 1);
	if (a.size() == 1)
		CHECK_STR(a[0].c_str(), "hello10-iom");
	CHECK_EQ(iomcopy::added_lines("x\n", "x\n").size(), 0);
}

} // namespace

int main()
{
	test_mountpoint();
	test_patch();
	test_added();
	CHECK_DONE();
}

// test_hello10_mapinfo.cpp - hello10's naming of QNX mapping flags
// (src/hello10/mapinfo.h), on flag words read from the unit.
#include "types.h"
#include "check.h"
#include "mapinfo.h"

#include <string>
#include <vector>

namespace {

// CHECK_STR keeps the pointer: the string must outlive the check.
void check_flags(u32 flags, const char *want)
{
	std::string got = mapinfo::flags_str(flags);
	CHECK_STR(got.c_str(), want);
}

void test_flags()
{
	// MMI3GApplication's text and data segments (DCMD_PROC_MAPINFO on the car)
	check_flags(0x00000571, "SHARED FIXED ELF NOSYNCFILE r-x");
	check_flags(0x00000371, "SHARED FIXED ELF NOSYNCFILE rw-");
	// a thread stack's guard page
	check_flags(0x00001000, "type0 STACK ---");
	// an unnamed bit shows as hex
	check_flags(0x10000002, "PRIVATE 0x10000000 ---");
}

void test_anon_private()
{
	// MMI3GApplication's thread stacks (126976 bytes each)
	std::string s = mapinfo::flags_str(0x01081782);
	CHECK_STR(s.c_str(), "PRIVATE LAZY STACK ANON SYSRAM rwx");
}

void test_verdict()
{
	std::string text = mapinfo::write_verdict(0x00000571);
	CHECK(text.find("SHARED") != std::string::npos);
	CHECK(text.find("ESRVRFAULT") != std::string::npos);
	std::string priv = mapinfo::write_verdict(0x00000502);
	CHECK(priv.find("never copies") != std::string::npos);
	std::string rw = mapinfo::write_verdict(0x00000371);
	CHECK(rw.find("writable") != std::string::npos);
}

void test_find()
{
	std::vector<mapinfo::Seg> segs(2);
	segs[0].vaddr = 0x08040000;
	segs[0].size = 0x9eb000;   // one page short of segs[1]: a gap
	segs[1].vaddr = 0x08a2c000;
	segs[1].size = 0x49000;
	CHECK(mapinfo::find(segs, 0x0818a190) == &segs[0]);
	CHECK(mapinfo::find(segs, 0x08a2c000) == &segs[1]);
	CHECK(mapinfo::find(segs, 0x08a2bfff) == nullptr);   // the gap between them
	CHECK(mapinfo::find(segs, 0x08a75000) == nullptr);   // just past the end
	CHECK(mapinfo::find(segs, 0x08040000 - 1) == nullptr);
}

} // namespace

int main()
{
	test_flags();
	test_anon_private();
	test_verdict();
	test_find();
	CHECK_DONE();
}

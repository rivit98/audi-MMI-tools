// test_carsig.cpp - src/common/camera/carsig.h: the car-signal change log
// (one line per function per gap, held changes counted) and the night state
// from DA01.0C0D's first byte, on payloads logged on the car.
#include "types.h"
#include "carsig.h"
#include "check.h"
#include "ipc4.h"

#include <string>
#include <vector>

namespace {

ipc4::Most most(u8 fblock, u16 fct, const std::vector<u8> &payload)
{
	ipc4::Most m;
	m.fblock = fblock;
	m.inst = 0x01;
	m.fct = fct;
	m.op = 0x0c;
	m.payload = payload;
	return m;
}

bool has(const std::vector<std::string> &v, const std::string &s)
{
	for (const std::string &x : v)
		if (x == s)
			return true;
	return false;
}

void test_night()
{
	carsig::Logger lg;
	std::vector<std::string> out;
	CHECK(lg.night() == carsig::Night::Unknown);
	CHECK_EQ(lg.dimming(), -1);

	lg.feed(1000, most(0xDA, 0x0C0D, {0x64, 0x64}), out);
	CHECK(lg.night() == carsig::Night::Night);
	CHECK_EQ(lg.dimming(), 100);
	CHECK(has(out, "night: unknown -> NIGHT (DA01.0C0D byte 0 = 100)"));
	CHECK(has(out, "DA01.0C0D/C dimming (night) first [64 64]"));

	out.clear();
	lg.feed(9000, most(0xDA, 0x0C0D, {0x00, 0x00}), out);
	CHECK(lg.night() == carsig::Night::Day);
	CHECK(has(out, "night: NIGHT -> day (DA01.0C0D byte 0 = 0)"));
	CHECK(has(out, "DA01.0C0D/C dimming (night) [64 64] -> [00 00]"));
	CHECK_EQ(lg.night_changes(), 2u);

	// the same day value again: nothing
	out.clear();
	lg.feed(20000, most(0xDA, 0x0C0D, {0x00, 0x00}), out);
	CHECK_EQ(out.size(), 0u);

	// the light functions: every change at once, however close
	out.clear();
	lg.feed(20100, most(0xDA, 0x0C0D, {0x00, 0x64}), out);
	lg.feed(20200, most(0xDA, 0x0C0D, {0x64, 0x64}), out);
	CHECK(has(out, "DA01.0C0D/C dimming (night) [00 00] -> [00 64]"));
	CHECK(has(out, "DA01.0C0D/C dimming (night) [00 64] -> [64 64]"));
	lg.feed(0, most(0xDA, 0x0C0C, {0x01}), out);
	lg.feed(100, most(0xDA, 0x0C0C, {0x02}), out);
	CHECK(has(out, "DA01.0C0C/C exterior light [01] -> [02]"));
}

void test_throttle()
{
	carsig::Logger lg(5000);
	std::vector<std::string> out;
	lg.feed(0, most(0xDA, 0x0C10, {0x02, 0x02}), out);
	CHECK_EQ(out.size(), 1u);

	// first change after the gap: at once
	out.clear();
	lg.feed(6000, most(0xDA, 0x0C10, {0x03, 0x02}), out);
	CHECK(has(out, "DA01.0C10/C driving state [02 02] -> [03 02]"));

	// three more inside the gap: held
	out.clear();
	lg.feed(6500, most(0xDA, 0x0C10, {0x04, 0x02}), out);
	lg.feed(7000, most(0xDA, 0x0C10, {0x05, 0x02}), out);
	lg.feed(8000, most(0xDA, 0x0C10, {0x00, 0x00}), out);
	CHECK_EQ(out.size(), 0u);
	lg.tick(10999, out);
	CHECK_EQ(out.size(), 0u);
	lg.tick(11000, out);
	CHECK(has(out, "DA01.0C10/C driving state [03 02] -> [00 00] (3 changes in 5.0 s)"));

	// a held change goes out at flush, too
	out.clear();
	lg.feed(12000, most(0xDA, 0x0C10, {0x01, 0x00}), out);
	CHECK_EQ(out.size(), 0u);
	lg.flush(12500, out);
	CHECK(has(out, "DA01.0C10/C driving state [00 00] -> [01 00] (1 changes in 1.5 s)"));

	// window counts every change, then resets
	std::string w = lg.window();
	CHECK(w == "night unknown (byte 0 -); changes: DA01.0C10/C driving state x5");
	CHECK(lg.window() == "night unknown (byte 0 -); changes: none");
}

void test_bap_left_out()
{
	carsig::Logger lg;
	std::vector<std::string> out;
	lg.feed(0, most(ipc4::kBapFblock, ipc4::kBapFct, {0x11, 0x11, 0x03, 0x00, 0x0c, 0x0e, 0x1d}),
	        out);
	CHECK_EQ(out.size(), 0u);
	CHECK_EQ(lg.keys(), 0u);
	lg.feed(0, most(0xDD, 0x0333, {0x00, 0x01}), out);
	CHECK(has(out, "DD01.0333/C cluster first [00 01]"));
}

} // namespace

int main()
{
	test_night();
	test_throttle();
	test_bap_left_out();
	CHECK_DONE();
}

// test_hello11.cpp - hello11's ch4 telegram parsing (src/hello11/ipc4.h) and
// its change / step bookkeeping (src/hello11/watch.h). The telegrams are
// built here in the layout ipc4.h describes.
#include "types.h"
#include "check.h"
#include "ipc4.h"
#include "watch.h"

#include <string>
#include <vector>

namespace {

// counter, type 1, MOST header + payload, FA DE
std::vector<u8> telegram(u8 counter, u8 fblock, u8 inst, u16 fct, const std::vector<u8> &payload)
{
	std::vector<u8> body = {0x00, fblock, inst, (u8)(fct >> 8), (u8)fct, 0x0c, 0x00,
	                        (u8)payload.size(), (u8)(payload.size() >> 8), 0xff};
	body.insert(body.end(), payload.begin(), payload.end());
	usize len = body.size() + 1;
	std::vector<u8> t = {counter, 0x01, (u8)len, (u8)(len >> 8)};
	t.insert(t.end(), body.begin(), body.end());
	t.push_back(0xfa);
	t.push_back(0xde);
	return t;
}

void append(std::vector<u8> &a, const std::vector<u8> &b)
{
	a.insert(a.end(), b.begin(), b.end());
}

void test_split()
{
	std::vector<u8> s;
	append(s, telegram(0x10, 0xd8, 0x01, 0x0500, {0x0f, 0x2a, 0x03, 0x00, 0x01, 0x02, 0x03}));
	append(s, telegram(0x11, 0xda, 0x01, 0x0c0d, {0x01, 0x00}));
	append(s, telegram(0x12, 0xfb, 0x01, 0x0d06, {0x70}));

	// one byte at a time: the same telegrams
	ipc4::Splitter sp;
	std::vector<ipc4::Telegram> out;
	for (u8 b : s)
		sp.feed(&b, 1, out);
	CHECK_EQ(out.size(), 3);
	CHECK_EQ(sp.gaps(), 0);
	CHECK_EQ(sp.skipped(), 0);
	CHECK_EQ(out[1].counter, 0x11);
	CHECK_EQ(out[1].body.size(), 12);

	ipc4::Most m;
	CHECK(ipc4::parse_most(out[1].body, m));
	CHECK_EQ(m.fblock, ipc4::kCarFblock);
	CHECK_EQ(m.fct, ipc4::kInteriorLightFct);
	CHECK_EQ(m.op, 0x0c);
	CHECK_EQ(m.payload.size(), 2);
	CHECK(ipc4::most_key(m) == std::string("DA01.0C0D/C"));

	CHECK(ipc4::parse_most(out[0].body, m));
	CHECK(ipc4::is_bap(m));
	std::vector<ipc4::BapFn> fns;
	CHECK(ipc4::parse_bap(m.payload, fns));
	CHECK_EQ(fns.size(), 1);
	CHECK_EQ(fns[0].lsg, 0x0f);
	CHECK_EQ(fns[0].fid, 0x2a);
	CHECK(ipc4::hex(fns[0].payload) == std::string("01 02 03"));
	CHECK(ipc4::bap_key(fns[0]) == std::string("BAP 0F.2A"));
}

void test_resync_and_gaps()
{
	std::vector<u8> s = {0x00, 0xfa, 0x13, 0x37};       // junk first
	append(s, telegram(0x20, 0xda, 0x01, 0x0c0a, {0x02}));
	append(s, telegram(0x23, 0xda, 0x01, 0x0c0a, {0x03}));   // 0x21, 0x22 missing
	append(s, telegram(0xff, 0xda, 0x01, 0x0c0a, {0x04}));   // a big jump
	append(s, telegram(0x00, 0xda, 0x01, 0x0c0a, {0x05}));   // wraps: no gap
	ipc4::Splitter sp;
	std::vector<ipc4::Telegram> out;
	sp.feed(s.data(), s.size(), out);
	CHECK_EQ(out.size(), 4);
	CHECK_EQ(sp.skipped(), 4);
	CHECK_EQ(sp.gaps(), 2);
	CHECK_EQ(sp.missing(), 2 + (0xff - 0x24));

	// a frame with a broken tail is skipped, the next one found
	std::vector<u8> bad = telegram(0x01, 0xda, 0x01, 0x0c0a, {0x01});
	bad.back() = 0x00;
	append(bad, telegram(0x02, 0xda, 0x01, 0x0c0a, {0x01}));
	ipc4::Splitter sp2;
	out.clear();
	sp2.feed(bad.data(), bad.size(), out);
	CHECK_EQ(out.size(), 1);
	CHECK_EQ(out[0].counter, 0x02);
}

void test_bap()
{
	// two functions of the camera's control unit in one stream
	std::vector<u8> s = {0x0b, 0x17, 0x02, 0x00, 0xff, 0x01, 0x13, 0x01, 0x00, 0x04};
	std::vector<ipc4::BapFn> fns;
	CHECK(ipc4::parse_bap(s, fns));
	CHECK_EQ(fns.size(), 2);
	CHECK_EQ(fns[0].fid, ipc4::kRvcPresentationFid);
	CHECK(ipc4::hex(fns[0].payload) == std::string("ff 01"));
	CHECK_EQ(fns[1].fid, 0x13);
	CHECK_EQ(fns[1].payload[0], 0x04);
	// a length past the end, lsg 0, too short
	std::vector<u8> cut = {0x0b, 0x17, 0x05, 0x00, 0xff};
	CHECK(!ipc4::parse_bap(cut, fns));
	CHECK(!ipc4::parse_bap({0x00, 0x17, 0x01, 0x00, 0x01}, fns));
	CHECK(!ipc4::parse_bap({0x0b, 0x17}, fns));
	// not MOST-shaped bodies
	ipc4::Most m;
	CHECK(!ipc4::parse_most({0x00, 0xda, 0x01}, m));
	CHECK(!ipc4::parse_most({0x00, 0xda, 0x01, 0x0c, 0x0a, 0x0c, 0x00, 0x05, 0x00, 0xff, 0x01}, m));
	CHECK(ipc4::hex({1, 2, 3, 4}, 2) == std::string("01 02 .."));
}

void test_watch()
{
	watch::Watch w;
	CHECK(!w.value(0, "noise", {0}));
	CHECK(!w.value(0, "light", {0}));
	CHECK(!w.value(0, "cam", {0x00, 0x00}));
	CHECK(!w.value(5, "light", {0}));                 // same value: no change

	w.begin(1000, "quiet", "");
	w.value(1500, "noise", {1});
	w.begin(2000, "on 1", "lights on");
	w.value(2500, "noise", {2});
	CHECK(w.value(2600, "light", {1}));
	w.begin(3000, "off 1", "lights off");
	w.value(3500, "light", {0});
	w.begin(4000, "on 2", "lights on");
	w.value(4500, "light", {1});
	w.begin(5000, "rev", "reverse");
	w.value(5500, "cam", {0xff, 0x01});
	w.value(5600, "light", {0});                      // lights changed here too
	w.end(6000);
	w.value(6500, "cam", {0x00, 0x00});               // after the last step: in none

	CHECK_EQ(w.steps().size(), 5);
	CHECK_EQ(w.steps()[1].end_ms, 3000);
	CHECK_EQ(w.changed_in(2000, 3000).size(), 2);

	auto c = w.candidates();
	CHECK_EQ(c["lights on"].size(), 1);
	CHECK(c["lights on"][0] == std::string("light"));       // noise changed in quiet too
	CHECK_EQ(c["lights off"].size(), 1);
	CHECK_EQ(c["reverse"].size(), 2);                    // cam and light
	std::vector<std::string> d = w.describe("cam", 5000, 6000);
	CHECK_EQ(d.size(), 1);
	CHECK(d[0] == std::string("[00 00] -> [ff 01]"));
}

} // namespace

int main()
{
	test_split();
	test_resync_and_gaps();
	test_bap();
	test_watch();
	CHECK_DONE();
}

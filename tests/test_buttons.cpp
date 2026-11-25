// test_buttons.cpp - src/common/buttons/: the decoder on ch8 messages logged on the
// car (README.md there), the hub's observers and the event queue.
#include "check.h"
#include "types.h"

#include "buttons.h"
#include "hub.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "log.h"
}

using namespace buttons;

namespace {

// A plain 27-byte message: constant bytes set, one byte changed.
std::vector<u8> msg(int byte = -1, int value = 0)
{
	std::vector<u8> m(kMsgBytes, 0);
	m[14] = 0x01;
	m[15] = 0x02;
	m[17] = 0x01;
	if (byte >= 0)
		m[byte] = (u8)value;
	return m;
}

// A touch pad message: bytes 14-15 = 02 02 and the extra bytes.
std::vector<u8> pad(std::vector<u8> extra)
{
	std::vector<u8> m = msg();
	m[14] = 0x02;
	m.insert(m.end(), extra.begin(), extra.end());
	return m;
}

std::vector<Event> dec(Decoder &d, const std::vector<u8> &m, u32 ms = 0)
{
	std::vector<Event> out;
	d.decode(m.data(), (int)m.size(), ms, out);
	return out;
}

// One button: press -> one Press event, release -> one Release event.
void button(int byte, int mask, Key key, const char *name)
{
	Decoder d;
	dec(d, msg());                                  // power baseline
	std::vector<Event> e = dec(d, msg(byte, mask), 100);
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].key == key);
	CHECK(e[0].action == Action::Press);
	CHECK_EQ(e[0].byte, byte);
	CHECK_EQ(e[0].mask, mask);
	CHECK_EQ(e[0].ms, 100);
	CHECK_STR(key_name(e[0].key), name);
	CHECK_EQ(d.held(byte), mask);
	e = dec(d, msg(), 300);
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].key == key);
	CHECK(e[0].action == Action::Release);
	CHECK_EQ(d.held(byte), 0);
}

void test_table()
{
	button(0, 0x01, Key::Radio, "RADIO");
	button(0, 0x02, Key::Media, "MEDIA");
	button(0, 0x08, Key::Tel, "TEL");
	button(0, 0x10, Key::Nav, "NAV");
	button(0, 0x40, Key::Car, "CAR");
	button(1, 0x01, Key::Next, "next track");
	button(1, 0x02, Key::SoftTopLeft, "soft key top left");
	button(1, 0x04, Key::SoftBottomLeft, "soft key bottom left");
	button(1, 0x08, Key::Prev, "previous track");
	button(1, 0x10, Key::SoftTopRight, "soft key top right");
	button(1, 0x20, Key::SoftBottomRight, "soft key bottom right");
	button(1, 0x40, Key::Back, "BACK");
	button(1, 0x80, Key::MainKnob, "main knob");
	button(2, 0x01, Key::VolumeKnob, "volume knob");
	button(3, 0x04, Key::Menu, "MENU");
	button(9, 0x10, Key::Unknown, "unknown");      // made up: a bit nobody named
	button(0, 0x04, Key::Unknown, "unknown");
}

void test_knobs()
{
	Decoder d;
	std::vector<Event> e;

	e = dec(d, msg(6, 1));                          // main knob 1 detent right
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].key == Key::MainKnob && e[0].action == Action::Turn);
	CHECK_EQ(e[0].detents, 1);
	CHECK(describe(e[0]) == "main knob turn +1");
	e = dec(d, msg(7, 1));
	CHECK_EQ(e[0].detents, -1);
	e = dec(d, msg(4, 3));                          // a fast turn: one message, 3 detents
	CHECK(e[0].key == Key::VolumeKnob);
	CHECK_EQ(e[0].detents, 3);
	e = dec(d, msg(5, 2));
	CHECK_EQ(e[0].detents, -2);
	CHECK(describe(e[0]) == "volume knob turn -2");

	// The faulty volume knob: both directions in one message, both reported
	std::vector<u8> m = msg(4, 1);
	m[5] = 1;
	e = dec(d, m);
	CHECK_EQ(e.size(), 2);
	CHECK_EQ(e[0].detents + e[1].detents, 0);

	// A knob message is not a button release, and the knob bytes hold no buttons
	e = dec(d, msg(1, 0x40));
	CHECK_EQ(e.size(), 1);
	m = msg(1, 0x40);
	m[6] = 2;
	e = dec(d, m);
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].action == Action::Turn);
	CHECK_EQ(d.held(1), 0x40);
	CHECK_EQ(d.held(6), 0);
}

void test_combinations()
{
	Decoder d;
	std::vector<Event> e;

	dec(d, msg());
	dec(d, msg(1, 0x40));                           // BACK held
	e = dec(d, msg(1, 0x40 | 0x80));                // + knob press
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].key == Key::MainKnob && e[0].action == Action::Press);
	e = dec(d, msg(1, 0x80));                       // BACK let go
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].key == Key::Back && e[0].action == Action::Release);
	// One message changing bits in two bytes: release first, then press
	std::vector<u8> m = msg(3, 0x04);
	e = dec(d, m);
	CHECK_EQ(e.size(), 2);
	CHECK(e[0].key == Key::MainKnob && e[0].action == Action::Release);
	CHECK(e[1].key == Key::Menu && e[1].action == Action::Press);
	CHECK(is_knob_press(Event{}) == false);

	// The unit-state bytes 16-26 change with nothing touched: no event
	m = msg(3, 0x04);
	m[16] = 0x03;
	m[23] = 0x12;
	m[25] = 0x0f;
	m[26] = 0x11;
	e = dec(d, m);
	CHECK_EQ(e.size(), 0);
}

void test_touch()
{
	Decoder d;
	std::vector<Event> e;

	dec(d, msg());
	dec(d, msg(1, 0x80));                           // knob held
	// The car's touch pad stroke: finger down, a move, finger up
	e = dec(d, pad({0xca, 0x84, 0xc6}));
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].key == Key::TouchPad && e[0].action == Action::Touch);
	CHECK_EQ(e[0].touch_len, 3);
	CHECK_EQ(e[0].touch[0], 0xca);
	CHECK(describe(e[0]) == "touch pad ca 84 c6");
	CHECK_EQ(d.held(1), 0x80);                      // a pad message releases nothing
	e = dec(d, pad({0x3d, 0x3c}));
	CHECK_EQ(e[0].touch_len, 2);
	e = dec(d, pad({0xcb}));
	CHECK_EQ(e[0].touch_len, 1);
	// The c4 pair after a knob press, and a pad message with no extra bytes
	e = dec(d, pad({0xc4, 0x06}));
	CHECK_EQ(e.size(), 1);
	std::vector<u8> m = msg();
	m[14] = 0x02;
	e = dec(d, m);
	CHECK_EQ(e.size(), 0);
	// Long extras are cut at kTouchMax
	e = dec(d, pad({1, 2, 3, 4, 5, 6, 7, 8}));
	CHECK_EQ(e[0].touch_len, kTouchMax);
}

void test_power()
{
	Decoder d;
	std::vector<Event> e;

	e = dec(d, msg());                              // 02: the baseline, no event
	CHECK_EQ(e.size(), 0);
	// The sequence seen when the car switched off: 02 02 (pad), 01 03, 01 00
	e = dec(d, pad({0xc4, 0x00}));
	CHECK_EQ(e.size(), 1);
	std::vector<u8> m = msg();
	m[15] = 0x03;
	e = dec(d, m);
	CHECK_EQ(e.size(), 1);
	CHECK(e[0].action == Action::Power);
	CHECK_EQ(e[0].power, 0x03);
	CHECK(describe(e[0]) == "power 03");
	m[15] = 0x00;
	e = dec(d, m);
	CHECK_EQ(e[0].power, 0x00);

	// A first message already on the way down is reported
	Decoder d2;
	m[15] = 0x03;
	e = dec(d2, m);
	CHECK_EQ(e.size(), 1);

	// Too short: nothing, no crash
	Decoder d3;
	u8 tiny[8] = {0, 0x80};
	std::vector<Event> out;
	d3.decode(tiny, 8, 0, out);
	d3.decode(tiny, 0, 0, out);
	CHECK_EQ(out.size(), 0);

	d.reset();
	CHECK_EQ(dec(d, msg()).size(), 0);
}

void feed(Hub &h, const std::vector<u8> &m, u32 ms = 0)
{
	h.feed(m.data(), (int)m.size(), ms);
}

void test_hub()
{
	Hub hub;
	std::vector<std::string> a, b;
	int ia = hub.subscribe([&](const Event &e) { a.push_back(describe(e)); });
	int ib = hub.subscribe([&](const Event &e) { b.push_back(describe(e)); });
	CHECK(ia > 0 && ib > 0 && ia != ib);

	feed(hub, msg(6, 2));
	feed(hub, msg(1, 0x80));
	CHECK_EQ(hub.held(1), 0x80);
	hub.unsubscribe(ib);
	feed(hub, msg());
	hub.unsubscribe(12345);                         // unknown id: nothing
	CHECK_EQ(a.size(), 3);
	CHECK_EQ(b.size(), 2);
	CHECK_STR(a[0].c_str(), "main knob turn +2");
	CHECK_STR(a[1].c_str(), "main knob press");
	CHECK_STR(a[2].c_str(), "main knob release");
	CHECK_EQ(hub.messages(), 3);

	// the ipc.h callback form
	Hub::ipc_cb(8, 0, msg(7, 1).data(), kMsgBytes, &hub);
	CHECK_EQ(a.size(), 4);
	CHECK_STR(a[3].c_str(), "main knob turn -1");
}

void test_queue()
{
	Hub hub;
	Event e;
	{
		EventQueue q(hub, 3);
		CHECK(!q.poll(e));
		CHECK(!q.wait(e, 20));                      // times out
		feed(hub, msg(6, 1), 10);
		feed(hub, msg(1, 0x80), 20);
		CHECK_EQ(q.size(), 2);
		CHECK(q.poll(e));
		CHECK(e.action == Action::Turn && e.ms == 10);
		CHECK(q.wait(e, 1000));
		CHECK(is_knob_press(e));

		// Full: the oldest go
		for (int i = 1; i <= 5; i++)
			feed(hub, msg(6, i));
		CHECK_EQ(q.size(), 3);
		CHECK_EQ(q.dropped(), 2 + 1);               // the release + 2 turns
		CHECK(q.poll(e));
		CHECK_EQ(e.detents, 3);
		q.clear();
		CHECK_EQ(q.size(), 0);

		// Another thread feeds while we wait
		std::thread t([&] { feed(hub, msg(3, 0x04), 99); });
		CHECK(q.wait(e, 5000));
		CHECK(e.key == Key::Menu);
		t.join();
	}
	// The queue is gone: feeding must not touch it
	feed(hub, msg(6, 1));
	feed(hub, msg());
}

} // namespace

// The hub's log lines, without the "[   N ms] " stamp.
std::vector<std::string> g_lines;

void tap(const char *line)
{
	const char *p = std::strstr(line, "] ");
	g_lines.push_back(p ? p + 2 : line);
}

void test_log()
{
	Hub hub;
	g_lines.clear();
	log_set_tap(tap);
	feed(hub, msg(1, 0x40));                        // nobody listens
	int id = hub.subscribe([](const Event &) {});
	{
		EventQueue q(hub, 1, "menu");
		feed(hub, msg());                           // BACK release
		feed(hub, msg(6, 1));                       // queue full: drops the release
	}
	hub.unsubscribe(id);
	hub.unsubscribe(777);
	hub.set_logging(false);
	feed(hub, msg(6, 1));
	log_set_tap(nullptr);

	const char *want[] = {
		"buttons: BACK press -> nobody",
		"buttons: subscribe #1 (id 1, 1 subscribers)",
		"buttons: subscribe menu (id 2, 2 subscribers)",
		"buttons: BACK release -> #1, menu",
		"buttons: menu full (1), dropped BACK release (1 dropped so far)",
		"buttons: main knob turn +1 -> #1, menu",
		"buttons: unsubscribe menu (id 2, 1 subscribers)",
		"buttons: unsubscribe #1 (id 1, 0 subscribers)",
		"buttons: unsubscribe id 777: no such subscriber",
	};
	CHECK_EQ(g_lines.size(), sizeof(want) / sizeof(want[0]));
	for (usize i = 0; i < g_lines.size() && i < sizeof(want) / sizeof(want[0]); i++)
		CHECK_STR(g_lines[i].c_str(), want[i]);
}

int main()
{
	test_table();
	test_knobs();
	test_combinations();
	test_touch();
	test_power();
	test_hub();
	test_queue();
	test_log();
	CHECK_DONE();
}

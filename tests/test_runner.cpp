// test_runner.cpp - src/runner/: apps.conf parsing, the verdicts, and the
// knob-driven menu.
#include "check.h"
#include "types.h"

#include "conf.h"
#include "menu.h"

#include <string>

using namespace runner;

namespace {

void test_conf()
{
	const std::string text =
		"# a comment\n"
		"\n"
		"hello|30||Nothing on screen.\n"
		"hello4|160|step=10 trace=1|Probes; if the picture stops: reboot.\n"
		"hello5|540|pd|text with | a bar | in it\r\n"
		"hello7||\n"
		"bad||\n"
		"|10|x|no name\n"
		"hello|45|again|replaced\n"
		"last|7";                               // no newline at the end
	std::vector<AppConf> a = parse_conf(text);

	CHECK_EQ(a.size(), 6);
	CHECK_STR(a[0].name.c_str(), "hello");      // replaced, kept its place
	CHECK_EQ(a[0].timeout_s, 45);
	CHECK_EQ(a[0].args.size(), 1);
	CHECK_STR(a[0].args[0].c_str(), "again");
	CHECK_STR(a[0].text.c_str(), "replaced");

	CHECK_STR(a[1].name.c_str(), "hello4");
	CHECK_EQ(a[1].timeout_s, 160);
	CHECK_EQ(a[1].args.size(), 2);
	CHECK_STR(a[1].args[0].c_str(), "step=10");
	CHECK_STR(a[1].args[1].c_str(), "trace=1");
	CHECK_STR(a[1].text.c_str(), "Probes; if the picture stops: reboot.");

	CHECK_STR(a[2].text.c_str(), "text with | a bar | in it");   // CR dropped too
	CHECK_STR(a[2].args[0].c_str(), "pd");

	CHECK_STR(a[3].name.c_str(), "hello7");     // empty timeout: 120
	CHECK_EQ(a[3].timeout_s, 120);
	CHECK_EQ(a[3].args.size(), 0);
	CHECK_STR(a[4].name.c_str(), "bad");
	CHECK_STR(a[5].name.c_str(), "last");
	CHECK_EQ(a[5].timeout_s, 7);
	CHECK_STR(a[5].text.c_str(), "");

	CHECK_EQ(parse_conf("").size(), 0);
	CHECK_EQ(parse_conf("x|-5\n")[0].timeout_s, 120);
}

void test_verdict()
{
	CHECK_STR(verdict(false, false, 127, 0), "MISSING");
	CHECK_STR(verdict(true, true, 143, 0), "TIMEOUT");
	CHECK_STR(verdict(true, false, 139, 0), "CRASH");
	CHECK_STR(verdict(true, false, 2, 0), "FAIL");
	CHECK_STR(verdict(true, false, 0, 3), "WARN");
	CHECK_STR(verdict(true, false, 0, 0), "OK");
	CHECK_STR(verdict(true, false, 128, 0), "FAIL");   // only > 128 is a signal

	CHECK_EQ(count_fail_lines(""), 0);
	CHECK_EQ(count_fail_lines("ok\nFAIL x\n"), 1);
	CHECK_EQ(count_fail_lines("FAIL a FAIL b\nno\nFAIL"), 2);   // per line, last unterminated
	CHECK_EQ(count_fail_lines("fail\nFAILED\n"), 1);
}

void test_discovery()
{
	CHECK(valid_name("hello7"));
	CHECK(valid_name("my_app-2.1"));
	CHECK(!valid_name(""));
	CHECK(!valid_name(".hidden"));
	CHECK(!valid_name("._hello7"));               // macOS resource fork files
	CHECK(!valid_name("two words"));
	CHECK(!valid_name("x;rm"));
	CHECK(!valid_name("it's"));
	CHECK(!valid_name(std::string(65, 'a')));
	CHECK(valid_name(std::string(64, 'a')));

	const u8 elf[] = {0x7f, 'E', 'L', 'F', 1};
	const u8 txt[] = {'#', '!', '/', 'b'};
	CHECK(is_elf(elf, sizeof(elf)));
	CHECK(!is_elf(elf, 3));
	CHECK(!is_elf(txt, sizeof(txt)));

	std::vector<AppConf> conf = parse_conf(
		"hello5|540|pd|text\n"
		"hello7|940||text\n"
		"hello11|1860||Camera\n");
	// found on the card: two described, two not (any order from readdir)
	std::vector<AppConf> m = menu_apps({"zeta", "hello11", "alpha", "hello5", "alpha"}, conf);
	CHECK_EQ(m.size(), 4);
	CHECK_STR(m[0].name.c_str(), "hello5");     // apps.conf's order first
	CHECK_STR(m[0].args[0].c_str(), "pd");
	CHECK_EQ(m[0].timeout_s, 540);
	CHECK_STR(m[1].name.c_str(), "hello11");
	CHECK_STR(m[1].text.c_str(), "Camera");
	CHECK_STR(m[2].name.c_str(), "alpha");      // then the rest by name, once
	CHECK_STR(m[2].text.c_str(), "");
	CHECK_EQ(m[2].timeout_s, kDefaultTimeoutS);
	CHECK_EQ(m[2].args.size(), 0);
	CHECK_STR(m[3].name.c_str(), "zeta");
	// hello7 is described but not on the card: not listed
	CHECK_EQ(menu_apps({}, conf).size(), 0);
	CHECK_EQ(menu_apps({"x"}, {}).size(), 1);
}

buttons::Event turn(int detents)
{
	buttons::Event e;
	e.key = buttons::Key::MainKnob;
	e.action = buttons::Action::Turn;
	e.detents = detents;
	return e;
}

buttons::Event ev(buttons::Key k, buttons::Action a)
{
	buttons::Event e;
	e.key = k;
	e.action = a;
	return e;
}

void test_menu()
{
	using A = Menu::Act;
	using buttons::Action;
	using buttons::Key;
	Menu m(10, 4);

	CHECK_EQ(m.selected(), 0);
	CHECK(m.on_event(turn(-1)) == A::Moved);      // up from the top: the last
	CHECK_EQ(m.selected(), 9);
	CHECK_EQ(m.top(), 6);
	CHECK(m.on_event(turn(1)) == A::Moved);        // and back down: the first
	CHECK_EQ(m.selected(), 0);
	CHECK_EQ(m.top(), 0);
	CHECK(m.on_event(turn(1)) == A::Moved);
	CHECK_EQ(m.selected(), 1);
	CHECK_EQ(m.top(), 0);
	CHECK(m.on_event(turn(3)) == A::Moved);        // a fast turn: 3 at once
	CHECK_EQ(m.selected(), 4);
	CHECK_EQ(m.top(), 1);                          // the window follows
	CHECK(m.on_event(turn(5)) == A::Moved);
	CHECK_EQ(m.selected(), 9);
	CHECK_EQ(m.top(), 6);
	CHECK(m.on_event(turn(3)) == A::Moved);        // past the end: wraps
	CHECK_EQ(m.selected(), 2);
	CHECK_EQ(m.top(), 2);
	CHECK(m.on_event(turn(10)) == A::None);        // a whole round: back where it was
	CHECK(m.on_event(turn(-25)) == A::Moved);      // more than a round up
	CHECK_EQ(m.selected(), 7);
	CHECK(m.on_event(turn(5)) == A::Moved);
	CHECK_EQ(m.selected(), 2);

	CHECK(m.on_event(ev(Key::MainKnob, Action::Press)) == A::Start);
	CHECK(m.on_event(ev(Key::MainKnob, Action::Release)) == A::None);
	CHECK(m.on_event(ev(Key::Back, Action::Press)) == A::None);
	buttons::Event vol = turn(2);
	vol.key = Key::VolumeKnob;                     // the volume knob doesn't move it
	CHECK(m.on_event(vol) == A::None);
	CHECK_EQ(m.selected(), 2);

	m.select(-3);
	CHECK_EQ(m.selected(), 0);
	m.select(99);
	CHECK_EQ(m.selected(), 9);

	Menu empty(0, 4);
	CHECK(empty.on_event(ev(Key::MainKnob, Action::Press)) == A::None);
	CHECK(empty.on_event(turn(1)) == A::None);
	Menu one(1, 0);                                // rows 0 treated as 1
	CHECK(one.on_event(turn(5)) == A::None);
	CHECK_EQ(one.top(), 0);
}

} // namespace

int main()
{
	test_conf();
	test_verdict();
	test_discovery();
	test_menu();
	CHECK_DONE();
}

// test_camera.cpp - common/camera_log.h: the camera's state from the slog, and
// what an app with a picture does about it.
#include "camera_log.h"
#include "check.h"

#include <cstring>
#include <vector>

using camera::Takeover;
using camera::Tracker;
using Action = camera::Takeover::Action;

namespace {

void feed(Tracker &t, const char *text, unsigned now)
{
	t.line(text, std::strlen(text), now);
}

// One event as slogger stores it.
void add_event(std::vector<int> &ev, const char *text, bool is_text = true)
{
	size_t bytes = std::strlen(text) + 1;
	size_t ints = (bytes + 3) / 4;
	ev.push_back((int)((ints << 16) | (is_text ? 0x8 : 0) | 5));
	ev.push_back(20001);
	ev.push_back(0);
	size_t at = ev.size();
	ev.resize(at + ints, 0);
	std::memcpy(&ev[at], text, bytes);
}

void test_tracker()
{
	Tracker t;
	CHECK(!t.active());
	// our own stream: not the camera
	feed(t, "dev-videoctrl: VP1: source changed to 1", 10);
	feed(t, "dev-videoctrl: VP1: vcap enabled", 10);
	CHECK(!t.active());
	feed(t, "dev-videoctrl: VP1: source changed to 0", 20);
	feed(t, "dev-videoctrl: VP1: vcap disabled", 20);
	CHECK(!t.active());
	CHECK(!t.ended());
	// the sequence every camera event logs
	feed(t, "dev-videoctrl: VP0: hue changed to 0", 100);
	CHECK(!t.active());
	feed(t, "dev-videoctrl: VP0: source changed to 1", 100);
	CHECK(t.active());
	feed(t, "dev-videoctrl: VP1: source changed to 0", 100);
	feed(t, "dev-videoctrl: VP1: vcap enabled", 100);
	feed(t, "dev-videoctrl: VP1: mode changed to WEAVE", 100);
	CHECK(t.active());
	feed(t, "dev-videoctrl: VP1: vcap disabled", 900);
	CHECK(!t.active());
	CHECK(t.ended());
	CHECK(t.ended_ms() == 900);
	// the next session
	feed(t, "dev-videoctrl: VP0: source changed to 1", 1000);
	CHECK(t.active());
}

void test_events()
{
	std::vector<int> ev;
	add_event(ev, "MEM: 166780K, CPU: 9%");
	add_event(ev, "VP0: source changed to 1", false);  // binary event: not looked at
	Tracker t;
	t.events(ev.data(), ev.size(), 1);
	CHECK(!t.active());

	add_event(ev, "dev-videoctrl: VP0: source changed to 1");
	t.events(ev.data(), ev.size(), 2);
	CHECK(t.active());

	std::vector<int> off;
	add_event(off, "dev-videoctrl: VP1: vcap disabled");
	// cut short: nothing beyond the end is read
	t.events(off.data(), off.size() - 1, 3);
	CHECK(t.active());
	t.events(off.data(), off.size(), 4);
	CHECK(!t.active());
	CHECK(t.ended_ms() == 4);
}

void test_takeover_normal()
{
	Takeover o;
	// no camera: nothing to do
	CHECK(o.tick(true, false, true, 0) == Action::None);
	// camera up: nothing to write, the picture is taken
	CHECK(o.tick(true, true, false, 100) == Action::None);
	CHECK(o.taken());
	CHECK(o.tick(true, true, false, 8000) == Action::None);
	// off, not settled yet
	CHECK(o.tick(true, false, false, 8100) == Action::None);
	CHECK(o.taken());
	// settled
	CHECK(o.tick(true, false, true, 9100) == Action::Retake);
	CHECK(!o.taken());
	CHECK(o.tick(true, false, true, 9200) == Action::None);
	CHECK(o.tick(true, false, true, 9100 + camera::kShowAgainMs) == Action::ShowAgain);
	CHECK(o.tick(true, false, true, 20000) == Action::None);
}

void test_takeover_camera_comes_back()
{
	Takeover o;
	o.tick(true, true, false, 0);
	// released and engaged again before it settled: no retake
	CHECK(o.tick(true, false, false, 500) == Action::None);
	CHECK(o.tick(true, true, false, 700) == Action::None);
	CHECK(o.tick(true, false, true, 5000) == Action::Retake);
	// a later session, long after the retake: nothing written over it
	CHECK(o.tick(true, true, false, 5000 + camera::kVerifyMs) == Action::None);
	CHECK(o.taken());
	CHECK(o.tick(true, false, true, 20000) == Action::Retake);
}

void test_takeover_race()
{
	Takeover o;
	o.tick(true, true, false, 0);
	CHECK(o.tick(true, false, true, 2000) == Action::Retake);
	// the camera shows up right after our writes
	CHECK(o.tick(true, true, false, 2100) == Action::GiveBack);
	CHECK(o.taken());
	CHECK(o.tick(true, true, false, 2200) == Action::None);
	// no late show meant for the old retake
	CHECK(o.tick(true, false, false, 4200) == Action::None);
	CHECK(o.tick(true, false, true, 6000) == Action::Retake);
}

void test_takeover_no_picture()
{
	Takeover o;
	// camera with no picture of ours: nothing to come back to
	CHECK(o.tick(false, true, false, 0) == Action::None);
	CHECK(!o.taken());
	CHECK(o.tick(false, false, true, 2000) == Action::None);
	CHECK(o.tick(true, false, true, 2100) == Action::None);

	// the stream ended during the camera: nothing comes back either
	o.tick(true, true, false, 3000);
	CHECK(o.taken());
	CHECK(o.tick(false, true, false, 3100) == Action::None);
	CHECK(o.tick(false, false, true, 5000) == Action::None);
	CHECK(!o.taken());
	CHECK(o.tick(true, false, true, 5100) == Action::None);
}

} // namespace

int main()
{
	test_tracker();
	test_events();
	test_takeover_normal();
	test_takeover_camera_comes_back();
	test_takeover_race();
	test_takeover_no_picture();
	CHECK_DONE();
}

// test_camera.cpp - src/common/camera's pure parts: the camera's state from
// the slog (vcap.h) and from BAP on ch4 (bap.h, on telegrams logged on the
// car), the two combined (state.h), and what an app with a picture does
// about it (takeover.h).
#include "types.h"
#include "bap.h"
#include "check.h"
#include "ipc4.h"
#include "state.h"
#include "takeover.h"
#include "vcap.h"

#include <cstring>
#include <vector>

using camera::Combiner;
using camera::Rvc;
using camera::Takeover;
using Tracker = camera::VcapTracker;
using Action = camera::Takeover::Action;

namespace {

void feed(Tracker &t, const char *text, unsigned now)
{
	t.line(text, std::strlen(text), now);
}

// One event as slogger stores it.
void add_event(std::vector<int> &ev, const char *text, bool is_text = true)
{
	usize bytes = std::strlen(text) + 1;
	usize ints = (bytes + 3) / 4;
	ev.push_back((int)((ints << 16) | (is_text ? 0x8 : 0) | 5));
	ev.push_back(20001);
	ev.push_back(0);
	usize at = ev.size();
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

// One ch4 telegram as it comes off the channel: counter, type 1, len,
// the MOST header D8 01 0500 op C, the BAP stream, FA DE.
std::vector<u8> bap_telegram(u8 counter, const std::vector<u8> &stream)
{
	std::vector<u8> body = {0x00, 0xD8, 0x01, 0x05, 0x00, 0x0C, 0x00,
	                        (u8)stream.size(), (u8)(stream.size() >> 8), 0xFF};
	body.insert(body.end(), stream.begin(), stream.end());
	std::vector<u8> t = {counter, 0x01, (u8)(body.size() + 1), (u8)((body.size() + 1) >> 8)};
	t.insert(t.end(), body.begin(), body.end());
	t.push_back(0xFA);
	t.push_back(0xDE);
	return t;
}

Rvc rvc_of(const std::vector<u8> &raw)
{
	ipc4::Splitter sp;
	std::vector<ipc4::Telegram> ts;
	sp.feed(raw.data(), raw.size(), ts);
	CHECK_EQ(ts.size(), 1);
	return ts.empty() ? Rvc::None : camera::rvc_state(ts[0]);
}

void test_bap()
{
	// one camera session as the car sent it (BAP 0B.1A, 0B.16)
	CHECK(rvc_of(bap_telegram(0x98, {0x0b, 0x1a, 0x04, 0x00, 0xf1, 0x01, 0x01, 0x01})) == Rvc::On);
	CHECK(rvc_of(bap_telegram(0x9b, {0x0b, 0x16, 0x01, 0x00, 0x38})) == Rvc::None);
	CHECK(rvc_of(bap_telegram(0x9d, {0x0b, 0x1a, 0x04, 0x00, 0x11, 0x01, 0x01, 0x01})) == Rvc::On);
	CHECK(rvc_of(bap_telegram(0xbb, {0x0b, 0x1a, 0x04, 0x00, 0xf0, 0x00, 0x00, 0x00})) == Rvc::Off);
	CHECK(rvc_of(bap_telegram(0xbf, {0x0b, 0x1a, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00})) == Rvc::Off);
	// other control units and functions: parking aid 0A.13, the clock 11.11
	CHECK(rvc_of(bap_telegram(0x10, {0x0a, 0x13, 0x06, 0x00, 0xff, 0x4f, 0x51, 0xff, 0x01, 0x10}))
	      == Rvc::None);
	CHECK(rvc_of(bap_telegram(0x11, {0x11, 0x11, 0x03, 0x00, 0x0d, 0x0e, 0x1d})) == Rvc::None);
	// several functions in one stream: the last camera state counts
	CHECK(rvc_of(bap_telegram(0x12, {0x0b, 0x1a, 0x01, 0x00, 0x11, 0x16, 0x01, 0x00, 0x38,
	                                 0x1a, 0x01, 0x00, 0x00})) == Rvc::Off);
	// not BAP: the light status DA01.0C0D
	std::vector<u8> body = {0x00, 0xDA, 0x01, 0x0C, 0x0D, 0x0C, 0x00, 0x02, 0x00, 0xFF, 0x45, 0x64};
	std::vector<u8> t = {0x20, 0x01, (u8)(body.size() + 1), 0x00};
	t.insert(t.end(), body.begin(), body.end());
	t.push_back(0xFA);
	t.push_back(0xDE);
	CHECK(rvc_of(t) == Rvc::None);
	// an empty payload is no state
	ipc4::BapFn f;
	f.lsg = 0x0b;
	f.fid = 0x1a;
	CHECK(camera::rvc_state(f) == Rvc::None);
}

void test_combiner_session()
{
	// the car's timing: BAP ~1 s ahead of the slog both ways
	Combiner c;
	CHECK(!c.active());
	CHECK(c.clear(0));
	CHECK(c.bap(true, 58764));             // early warning
	CHECK(c.active());
	CHECK(!c.clear(58800));
	CHECK(!c.bap(true, 58863));            // f1 -> 11: still on
	CHECK(!c.vcap(true, 60028));           // the HMI takes VCAP 0
	CHECK(!c.tick(70000));                 // confirmed: no timeout
	CHECK(!c.bap(false, 65128));           // BAP off first: VCAP 0 still the HMI's
	CHECK(c.active());
	CHECK(c.vcap(false, 65713));
	CHECK(!c.active());
	CHECK(c.ended_ms() == 65713);
	CHECK(!c.clear(65713 + camera::kSettleMs - 1));
	CHECK(c.clear(65713 + camera::kSettleMs));
	CHECK_EQ(c.unconfirmed(), 0);
}

void test_combiner_lost_telegrams()
{
	// lost "off": the HMI's release ends the session
	Combiner c;
	c.bap(true, 1000);
	c.vcap(true, 2000);
	CHECK(c.vcap(false, 30000));
	CHECK(!c.active());
	CHECK(!c.bap_on());

	// lost "on": the slog alone, as before
	Combiner d;
	CHECK(d.vcap(true, 1000));
	CHECK(d.active());
	CHECK(!d.bap(false, 5000));            // BAP off while VCAP 0 is held
	CHECK(d.active());
	CHECK(d.vcap(false, 6000));

	// an "on" the HMI never follows: dropped after kBapAloneMs
	Combiner e;
	e.bap(true, 1000);
	CHECK(!e.tick(1000 + camera::kBapAloneMs - 1));
	CHECK(e.active());
	CHECK(e.tick(1000 + camera::kBapAloneMs));
	CHECK(!e.active());
	CHECK_EQ(e.unconfirmed(), 1);
	CHECK(e.ended());
	// BAP repeats "on" (f1 -> 11) while dropped: a new session from that time
	CHECK(e.bap(true, 7000));
	CHECK(!e.tick(7000 + camera::kBapAloneMs - 1));

	// the HMI comes after the BAP "on" but before the timeout: confirmed
	Combiner g;
	g.bap(true, 0);
	g.vcap(true, 4000);
	CHECK(!g.tick(60000));
	CHECK(g.active());
	// a camera already up at start (slog), BAP arrives later: no timeout
	Combiner h;
	h.vcap(true, 0);
	h.bap(true, 100);
	CHECK(!h.tick(60000));
	CHECK(h.active());
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
	test_bap();
	test_combiner_session();
	test_combiner_lost_telegrams();
	test_takeover_normal();
	test_takeover_camera_comes_back();
	test_takeover_race();
	test_takeover_no_picture();
	CHECK_DONE();
}

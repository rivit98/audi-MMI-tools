// hello11 - can a native app tell (1) that the rear camera is wanted, (2)
// that the car's lights are on (the HMI's night map) and (3) that it is dark
// outside (the light sensor, the display's dimming), without the Java HMI?
// Read only: nothing is written to any channel or device.
//
// Sources, each value fed to watch::Watch under a key:
//   ch4    /dev/ipc/ch4, the IOC's telegrams to MMI3GApplication's MOST
//          subsystem (ipc4.h): every telegram's payload by function, and
//          every BAP function (rear camera: BAP 0B.17). The light flag is
//          expected in DA01.0C0D (InteriorLight, first byte).
//   ch5    /dev/ipc/ch5, the vehicle-signal records (sources.h), by tag;
//          RainSens (tag 0x09) may carry the light sensor
//   ndr    /dev/ndr/name/sensor/ReverseGear: ndr's reverse flag (from ch5)
//   slog   dev-videoctrl's lines (common/camera): the HMI shows the camera;
//          and any line about brightness, dimming or day / night (sources.h)
// dev-ipc gives every reader its own copy (docs/vehicle-data.md §2.2), as
// long as each reader keeps reading: both channel threads only read. As a
// guard, ch4 reading stops if our counters show gaps.
//
// The driver goes through steps (lights on / off twice, the light sensor
// covered / free twice with the switch at AUTO, reverse in / out twice,
// quiet steps before and after) and turns the main knob once at the end of
// each. The summary names the keys that changed in every step of a kind and
// in no quiet step.
//
// Output: <sd>/var/hello11.log (steps, the watched keys, summary),
// <sd>/var/hello11/ch4.txt (every telegram, decoded), ch4.bin (the raw bytes
// as read: u32 ms, u32 length, bytes), ch5.bin (the same for ch5), slog.txt
// (the brightness / day-night slog lines), changes.txt (every change of
// every key).
//
// usage: hello11 <sd_path>
#include "types.h"

extern "C" {
#include "lmgr.h"
#include "log.h"
#include "quit.h"
}

#include "camera.h"
#include "ipc4.h"
#include "sources.h"
#include "ui.h"
#include "watch.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr unsigned kWatchdogS = 1800;  // the intro's and steps' time limits add up to ~26 min
constexpr unsigned kQuietMs = 20000;
constexpr unsigned kLightMs = 60000;    // a step's time limit
constexpr unsigned kReverseMs = 150000; // the camera's follow-up time is up to ~90 s
constexpr unsigned kDarkMs = 120000;    // auto headlights come on within seconds
constexpr unsigned kBrightMs = 180000;  // and go off only after a delay
constexpr unsigned kScreenMs = 1000;
constexpr unsigned kPageMs = 30000;
constexpr unsigned kGapCheckAfter = 20; // telegrams
constexpr const char *kCh4 = "/dev/ipc/ch4";
constexpr const char *kCh5 = "/dev/ipc/ch5";
constexpr const char *kFooter = "hello11 - read only. Pull the SD card to stop.";
const char *const kNdrPaths[] = {"/dev/ndr/name/sensor/ReverseGear", "/dev/navi/sensor/ReverseGear"};

struct StepDef {
	const char *label;
	const char *kind;              // "" = quiet
	unsigned max_ms;
	bool click;                    // ends with a knob click (else: after max_ms)
	const char *text;
};

const StepDef kSteps[] = {
	{"quiet 1", "", kQuietMs, false,
	 "Parked, selector in P, ignition on, light switch at 0 (off). Touch nothing for 20 seconds."},
	{"lights on 1", "lights on", kLightMs, true,
	 "Turn the light switch to the headlights (low beam). Then turn the main knob ONE click."},
	{"lights off 1", "lights off", kLightMs, true,
	 "Turn the light switch back to 0 (off). Then turn the main knob ONE click."},
	{"lights on 2", "lights on", kLightMs, true,
	 "Headlights ON again. Then turn the main knob ONE click."},
	{"lights off 2", "lights off", kLightMs, true,
	 "Light switch to 0 again. Then turn the main knob ONE click."},
	{"switch to AUTO", "switch AUTO", kLightMs, true,
	 "Turn the light switch to AUTO. Then turn the main knob ONE click. "
	 "(The headlights stay off: it is light outside.)"},
	{"sensor covered 1", "sensor covered", kDarkMs, true,
	 "Cover the light sensor (top of the windscreen, behind the mirror) with your hand or a "
	 "cloth and keep it covered. When the headlights come on, wait 10 more seconds (does the "
	 "screen dim?), then turn the main knob ONE click, still covering."},
	{"sensor free 1", "sensor free", kBrightMs, true,
	 "Uncover the sensor. Wait until the headlights go OFF again (can take a minute or two), "
	 "then turn the main knob ONE click."},
	{"sensor covered 2", "sensor covered", kDarkMs, true,
	 "Once more: cover the sensor. Headlights on, 10 more seconds, then ONE click."},
	{"sensor free 2", "sensor free", kBrightMs, true,
	 "Uncover the sensor. Headlights off, then ONE click. Leave the switch at AUTO."},
	{"reverse 1", "reverse", kReverseMs, true,
	 "Foot on the brake, select R. When the camera picture is on, turn the main knob ONE click. "
	 "(This screen goes away while the camera is shown.)"},
	{"camera off 1", "camera off", kReverseMs, true,
	 "Select P. Wait until the camera picture is GONE (it can stay up to a minute or more), "
	 "then turn the main knob ONE click."},
	{"reverse 2", "reverse", kReverseMs, true,
	 "Once more: select R, and when the camera picture is on, ONE click."},
	{"camera off 2", "camera off", kReverseMs, true,
	 "Select P, wait until the camera picture is gone, then ONE click."},
	{"quiet 2", "", kQuietMs, false, "Touch nothing for 20 seconds."},
};

const char *sd_path;
std::string out_dir;

std::mutex mu;                          // everything below, between the threads
watch::Watch w;
std::string light_now = "?", rvc_now = "?", ndr_now = "?", rain_now = "?", slog_now = "-";
std::vector<std::string> text_q;        // -> ch4.txt
std::vector<std::string> change_q;      // -> changes.txt
std::vector<u8> raw_q;                  // -> ch4.bin
std::vector<u8> raw5_q;                 // -> ch5.bin
std::vector<std::string> slog_q;        // -> slog.txt
u32 ch5_reads = 0, ch5_records = 0, ch5_skipped = 0, slog_lines = 0;
std::string ch5_state = "not started";
u32 ch4_telegrams = 0, ch4_gaps = 0, ch4_missing = 0, ch4_skipped = 0, ch4_odd = 0;
std::string ch4_state = "not started";
std::atomic<unsigned> clicks{0};
std::atomic<bool> stopping{false};

void on_key(const u8 *buf, int n, void *)
{
	if (n > 7 && (buf[6] != 0 || buf[7] != 0))
		clicks++;
}

bool sleep_quit(unsigned ms)
{
	unsigned t0 = log_ms();
	while (quit_poll() == QUIT_NONE) {
		unsigned now = log_ms();
		if (now - t0 >= ms)
			return false;
		unsigned left = ms - (now - t0);
		usleep((left > 50 ? 50 : left) * 1000);
	}
	return true;
}

// Under mu. Watched keys go to the main log too.
void feed(unsigned ms, const std::string &key, const std::vector<u8> &v, bool to_log)
{
	if (!w.value(ms, key, v))
		return;
	const watch::Change &c = w.changes().back();
	char line[300];
	std::snprintf(line, sizeof(line), "[%7u ms] %-18s [%s] -> [%s]", ms, key.c_str(),
	              ipc4::hex(c.from, 24).c_str(), ipc4::hex(c.to, 24).c_str());
	change_q.push_back(line);
	if (to_log)
		log_msg("change: %s [%s] -> [%s]", key.c_str(), ipc4::hex(c.from, 24).c_str(),
		        ipc4::hex(c.to, 24).c_str());
}

// ---- ch4 ----

void handle(unsigned ms, const ipc4::Telegram &t)
{
	char line[400];
	ipc4::Most m;
	if (t.type != 1 || !ipc4::parse_most(t.body, m)) {
		ch4_odd++;
		std::snprintf(line, sizeof(line), "[%7u ms] #%02x type %u, %zu bytes: %s", ms, t.counter,
		              t.type, t.body.size(), ipc4::hex(t.body).c_str());
		text_q.push_back(line);
		feed(ms, "type " + std::to_string(t.type), t.body, false);
		return;
	}
	std::string key = ipc4::most_key(m);
	std::snprintf(line, sizeof(line), "[%7u ms] #%02x %s: %s", ms, t.counter, key.c_str(),
	              ipc4::hex(m.payload).c_str());
	text_q.push_back(line);
	if (!ipc4::is_bap(m)) {
		bool light = m.fblock == ipc4::kCarFblock &&
		             (m.fct == ipc4::kInteriorLightFct || m.fct == ipc4::kExteriorLightFct);
		feed(ms, key, m.payload, light);
		if (m.fblock == ipc4::kCarFblock && m.fct == ipc4::kInteriorLightFct && !m.payload.empty())
			light_now = ipc4::hex(m.payload, 4);
		return;
	}
	std::vector<ipc4::BapFn> fns;
	if (!ipc4::parse_bap(m.payload, fns))
		text_q.push_back("           (BAP stream does not parse completely)");
	for (const ipc4::BapFn &f : fns) {
		bool rvc = f.lsg == ipc4::kRvcLsg;
		feed(ms, ipc4::bap_key(f), f.payload, rvc);
		if (rvc && f.fid == ipc4::kRvcPresentationFid)
			rvc_now = ipc4::hex(f.payload, 4);
	}
}

void ch4_thread(int fd)
{
	ipc4::Splitter sp;
	std::vector<ipc4::Telegram> ts;
	u8 buf[4096];
	for (;;) {
		isize n = read(fd, buf, sizeof(buf));
		if (stopping)
			return;
		if (n < 0 && errno == EINTR)
			continue;
		unsigned ms = log_ms();
		if (n <= 0) {
			log_msg("ch4: read returned %d (%s): stop reading", (int)n,
			        n < 0 ? std::strerror(errno) : "end");
			std::lock_guard<std::mutex> lock(mu);
			ch4_state = "read failed";
			close(fd);
			return;
		}
		ts.clear();
		sp.feed(buf, (usize)n, ts);
		std::lock_guard<std::mutex> lock(mu);
		u32 hdr[2] = {ms, (u32)n};
		raw_q.insert(raw_q.end(), (const u8 *)hdr, (const u8 *)hdr + sizeof(hdr));
		raw_q.insert(raw_q.end(), buf, buf + n);
		for (const ipc4::Telegram &t : ts)
			handle(ms, t);
		ch4_telegrams = sp.telegrams();
		ch4_gaps = sp.gaps();
		ch4_missing = sp.missing();
		ch4_skipped = sp.skipped();
		// Every second telegram missing = shared, not copied: give them all back.
		if (sp.telegrams() >= kGapCheckAfter && sp.gaps() * 5 > sp.telegrams()) {
			log_msg("ch4: STOP: %u gaps (%u telegrams missing) in %u telegrams: dev-ipc shares "
			        "them between readers, MMI3GApplication would miss ours",
			        sp.gaps(), sp.missing(), sp.telegrams());
			ch4_state = "stopped: gaps";
			close(fd);
			return;
		}
		ch4_state = "reading";
	}
}

// ---- ch5 ----

void ch5_thread(int fd)
{
	u8 buf[4096];
	for (;;) {
		isize n = read(fd, buf, sizeof(buf));
		if (stopping)
			return;
		if (n < 0 && errno == EINTR)
			continue;
		unsigned ms = log_ms();
		if (n <= 0) {
			log_msg("ch5: read returned %d (%s): stop reading", (int)n,
			        n < 0 ? std::strerror(errno) : "end");
			std::lock_guard<std::mutex> lock(mu);
			ch5_state = "read failed";
			close(fd);
			return;
		}
		usize skipped = 0;
		std::vector<ch5::Record> rs = ch5::parse(buf, (usize)n, &skipped);
		std::lock_guard<std::mutex> lock(mu);
		u32 hdr[2] = {ms, (u32)n};
		raw5_q.insert(raw5_q.end(), (const u8 *)hdr, (const u8 *)hdr + sizeof(hdr));
		raw5_q.insert(raw5_q.end(), buf, buf + n);
		ch5_reads++;
		ch5_records += (u32)rs.size();
		ch5_skipped += (u32)skipped;
		for (const ch5::Record &r : rs) {
			if (ch5::noisy(r.tag))
				continue;
			// the sensor-ish tags go to the main log as they change
			bool watched = r.tag == 0x09 || r.tag == 0x0a || r.tag == 0x08;
			feed(ms, ch5::key(r), r.payload, watched);
			if (r.tag == 0x09)
				rain_now = ipc4::hex(r.payload, 8);
		}
		ch5_state = "reading";
	}
}

// ---- slog: brightness / day-night lines ----

void slog_thread()
{
	int fd = open("/dev/slog", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		log_msg("slog: /dev/slog: %s: no brightness lines", std::strerror(errno));
		return;
	}
	static int buf[4096];
	bool backlog = true;            // the lines from before we started: file only
	while (!stopping) {
		isize n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno != EAGAIN) {
			log_msg("slog: read: %s: stop", std::strerror(errno));
			break;
		}
		if (n <= 0) {
			backlog = false;
			usleep(250 * 1000);
			continue;
		}
		unsigned ms = log_ms();
		std::vector<std::string> ls = slogtext::lines(buf, (usize)n / sizeof(int));
		std::lock_guard<std::mutex> lock(mu);
		for (const std::string &l : ls) {
			if (!slogtext::interesting(l))
				continue;
			char t[40];
			std::snprintf(t, sizeof(t), "[%7u ms]%s ", ms, backlog ? " (before start)" : "");
			slog_q.push_back(t + l);
			if (backlog)
				continue;
			slog_lines++;
			slog_now = l.substr(0, 60);
			std::string v = slogtext::numbers(l);
			feed(ms, "slog " + slogtext::key(l).substr(0, 60), std::vector<u8>(v.begin(), v.end()),
			     false);
			log_msg("slog: %s", l.c_str());
		}
	}
	close(fd);
}

// ---- ndr ----

void ndr_thread()
{
	int fd = -1;
	const char *path = nullptr;
	for (const char *p : kNdrPaths) {
		fd = open(p, O_RDONLY);
		if (fd >= 0) {
			path = p;
			break;
		}
		log_msg("ndr: %s: %s", p, std::strerror(errno));
	}
	if (fd < 0) {
		std::lock_guard<std::mutex> lock(mu);
		ndr_now = "none";
		return;
	}
	bool first = true;
	while (!stopping) {
		u8 b[64];
		isize n = pread(fd, b, sizeof(b), 0);
		unsigned ms = log_ms();
		if (n < 0) {
			log_msg("ndr: read %s: %s: stop", path, std::strerror(errno));
			break;
		}
		if (first) {
			log_msg("ndr: %s: %d bytes: %s", path, (int)n, ipc4::hex(b, (usize)n).c_str());
			first = false;
		}
		// time u32, status u8, flags u8, value u8: leave the time out
		std::vector<u8> v(b + (n > 4 ? 4 : n), b + n);
		{
			std::lock_guard<std::mutex> lock(mu);
			feed(ms, "ndr ReverseGear", v, true);
			ndr_now = v.size() >= 3 ? std::to_string(v[2]) : ipc4::hex(v);
		}
		usleep(250 * 1000);
	}
	close(fd);
}

// ---- output files ----

void writer_thread()
{
	FILE *txt = std::fopen((out_dir + "/ch4.txt").c_str(), "w");
	FILE *chg = std::fopen((out_dir + "/changes.txt").c_str(), "w");
	FILE *bin = std::fopen((out_dir + "/ch4.bin").c_str(), "wb");
	FILE *bin5 = std::fopen((out_dir + "/ch5.bin").c_str(), "wb");
	FILE *slg = std::fopen((out_dir + "/slog.txt").c_str(), "w");
	bool last = false;
	while (!last) {
		last = stopping;
		usleep(500 * 1000);
		std::vector<std::string> t, c, sl;
		std::vector<u8> r, r5;
		{
			std::lock_guard<std::mutex> lock(mu);
			t.swap(text_q);
			c.swap(change_q);
			r.swap(raw_q);
			r5.swap(raw5_q);
			sl.swap(slog_q);
		}
		if (slg != nullptr) {
			for (const std::string &s : sl)
				std::fprintf(slg, "%s\n", s.c_str());
			std::fflush(slg);
		}
		if (bin5 != nullptr && !r5.empty()) {
			std::fwrite(r5.data(), 1, r5.size(), bin5);
			std::fflush(bin5);
		}
		if (txt != nullptr) {
			for (const std::string &s : t)
				std::fprintf(txt, "%s\n", s.c_str());
			std::fflush(txt);
		}
		if (chg != nullptr) {
			for (const std::string &s : c)
				std::fprintf(chg, "%s\n", s.c_str());
			std::fflush(chg);
		}
		if (bin != nullptr && !r.empty()) {
			std::fwrite(r.data(), 1, r.size(), bin);
			std::fflush(bin);
		}
	}
	for (FILE *f : {txt, chg, bin, bin5, slg})
		if (f != nullptr)
			std::fclose(f);
}

// ---- screen ----

std::string status()
{
	std::lock_guard<std::mutex> lock(mu);
	char s[600];
	std::snprintf(s, sizeof(s),
	              "Light flag DA01.0C0D: %s\nch5 RainSens: %s\nCamera BAP 0B.17: %s\n"
	              "ndr reverse: %s, slog camera: %s\nslog: %s\n"
	              "ch4: %s, %u telegrams, %u gaps; ch5: %s, %u records; %zu keys",
	              light_now.c_str(), rain_now.c_str(), rvc_now.c_str(), ndr_now.c_str(),
	              camera::active() ? "UP" : "off", slog_now.c_str(), ch4_state.c_str(),
	              ch4_telegrams, ch4_gaps, ch5_state.c_str(), ch5_records, w.keys());
	return s;
}

bool shown = false;

// Our screen, unless the HMI shows the camera (never hide it).
void screen(const std::string &title, const std::string &text, bool with_status = true)
{
	if (camera::active()) {
		if (shown) {
			ui::hide();
			shown = false;
			log_msg("screen: hidden, the camera is up");
		}
		return;
	}
	if (!camera::clear())
		return;
	ui::show(title.c_str(), with_status ? text + "\n\n" + status() : text, kFooter);
	shown = true;
}

void restore_screen()
{
	int cids[3] = {40, 16, 19};
	ui::hide();
	if (lm_open() != 0)
		return;
	lm_unregister_displayable(40);
	log_msg("restore: lm_show_displayables(0, {40,16,19}): rc=%d", lm_show_displayables(0, cids, 3));
	lm_close();
}

// ---- steps ----

constexpr unsigned kIntroMs = 90000;
const char kIntro[] =
	"Parked in P, ignition on, light switch at 0. \"Click\" = turn the main knob ONE notch.\n"
	"1. 20 s: touch nothing.\n"
	"2. Headlights ON, click. Switch to 0, click. Twice.\n"
	"3. Switch to AUTO, click. Cover the light sensor (behind the mirror) until the headlights "
	"come on + 10 s, click. Uncover until they go off, click. Twice.\n"
	"4. R: when the camera is on, click. P: when it is gone (can take 1-2 min), click. Twice.\n"
	"5. 20 s: touch nothing, then the results.\n"
	"Each step is also shown on screen. Click to start.";

// The whole test on one screen, until a click (or kIntroMs). false: stop requested.
bool intro()
{
	log_msg("== intro (until a knob click)");
	unsigned t0 = log_ms(), c0 = clicks, shown_at = 0;
	bool first = true;
	for (;;) {
		unsigned now = log_ms();
		if (first || now - shown_at >= kScreenMs) {
			screen("hello11: rear camera, lights, light sensor (~10 min)", kIntro, false);
			shown_at = now;
			first = false;
		}
		if (clicks != c0 || now - t0 >= kIntroMs) {
			log_msg("intro: %s after %u ms", clicks != c0 ? "click" : "timeout", now - t0);
			return true;
		}
		if (sleep_quit(100))
			return false;
	}
}

// false: stop requested.
bool run_step(int n, const StepDef &s)
{
	char title[80];
	std::snprintf(title, sizeof(title), "hello11 step %d/%zu: %s", n,
	              sizeof(kSteps) / sizeof(kSteps[0]), s.label);
	unsigned t0 = log_ms();
	{
		std::lock_guard<std::mutex> lock(mu);
		w.begin(t0, s.label, s.kind);
	}
	log_msg("== step %d: %s%s", n, s.label, s.click ? " (until a knob click)" : "");
	unsigned c0 = clicks, shown_at = 0;
	bool first = true;
	for (;;) {
		unsigned now = log_ms();
		if (first || now - shown_at >= kScreenMs) {
			screen(title, s.text);
			shown_at = now;
			first = false;
		}
		if (s.click && clicks != c0) {
			log_msg("step %d: knob click after %u ms", n, now - t0);
			break;
		}
		if (now - t0 >= s.max_ms) {
			log_msg("step %d: %s after %u ms", n, s.click ? "TIMEOUT, no click" : "done",
			        now - t0);
			break;
		}
		if (sleep_quit(100))
			return false;
	}
	std::lock_guard<std::mutex> lock(mu);
	w.end(log_ms());
	return true;
}

std::vector<std::string> summary()
{
	std::lock_guard<std::mutex> lock(mu);
	std::vector<std::string> out;
	char s[300];
	std::snprintf(s, sizeof(s), "ch4: %s, %u telegrams, %u gaps (%u missing), %u bytes skipped, "
	              "%u not MOST-shaped", ch4_state.c_str(), ch4_telegrams, ch4_gaps, ch4_missing,
	              ch4_skipped, ch4_odd);
	out.push_back(s);
	std::snprintf(s, sizeof(s), "ch5: %s, %u reads, %u records, %u bytes skipped; slog: %u lines",
	              ch5_state.c_str(), ch5_reads, ch5_records, ch5_skipped, slog_lines);
	out.push_back(s);
	for (const watch::Step &st : w.steps()) {
		std::vector<std::string> keys = w.changed_in(st.begin_ms, st.end_ms);
		std::string line = st.label + " (" + std::to_string((st.end_ms - st.begin_ms) / 1000) +
		                   " s): " + std::to_string(keys.size()) + " keys changed";
		log_msg("%s", line.c_str());
		for (const std::string &k : keys) {
			std::vector<std::string> d = w.describe(k, st.begin_ms, st.end_ms);
			log_msg("    %-18s x%zu  %s%s", k.c_str(), d.size(), d.empty() ? "" : d.front().c_str(),
			        d.size() > 1 ? (" ... " + d.back()).c_str() : "");
		}
	}
	for (const auto &[kind, keys] : w.candidates()) {
		std::string line = kind + ": ";
		if (keys.empty())
			line += "nothing changed only then";
		for (usize i = 0; i < keys.size(); i++)
			line += (i ? ", " : "") + keys[i];
		out.push_back(line);
		// the first step of that kind, how each candidate changed
		for (const watch::Step &st : w.steps()) {
			if (st.kind != kind)
				continue;
			for (const std::string &k : keys) {
				std::vector<std::string> d = w.describe(k, st.begin_ms, st.end_ms);
				if (!d.empty())
					out.push_back("   " + k + ": " + d.front());
			}
			break;
		}
	}
	return out;
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path>\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	out_dir = std::string(var_dir(sd_path)) + "/hello11";
	mkdir(out_dir.c_str(), 0777);
	std::string logpath = std::string(var_dir(sd_path)) + "/hello11.log";
	if (log_open(logpath.c_str()) != 0)
		std::fprintf(stderr, "hello11: cannot open %s, logging to stdout only\n", logpath.c_str());
	log_msg("hello11 start, pid=%d", (int)getpid());
	quit_init_ex(sd_path, kWatchdogS, on_key, nullptr);
	camera::start();

	std::vector<std::thread> threads;
	try {
		threads.emplace_back(writer_thread);
		threads.emplace_back(ndr_thread);
		threads.emplace_back(slog_thread);
		int fd5 = open(kCh5, O_RDONLY | O_CLOEXEC);
		if (fd5 < 0) {
			log_msg("ch5: open %s: %s", kCh5, std::strerror(errno));
			ch5_state = "no open";
		} else {
			ch5_state = "reading";
			std::thread(ch5_thread, fd5).detach();
		}
		int fd = open(kCh4, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			log_msg("ch4: open %s: %s", kCh4, std::strerror(errno));
			ch4_state = "no open";
		} else {
			ch4_state = "reading";
			// blocks in read() until a telegram comes: detached, exit() ends it
			std::thread(ch4_thread, fd).detach();
		}
	} catch (const std::system_error &e) {
		log_msg("FAIL thread: %s", e.what());
	}

	int n = 0;
	if (intro())
		for (const StepDef &s : kSteps)
			if (!run_step(++n, s))
				break;

	log_msg("== summary");
	std::vector<std::string> sum = summary();
	for (const std::string &s : sum)
		log_msg("  %s", s.c_str());
	if (quit_poll() == QUIT_NONE) {
		unsigned t0 = log_ms();
		while (log_ms() - t0 < kPageMs && quit_poll() == QUIT_NONE) {
			if (!camera::active() && camera::clear()) {
				ui::show_list("hello11 results (also in hello11.log)", sum);
				shown = true;
			} else if (shown) {
				ui::hide();
				shown = false;
			}
			if (sleep_quit(1000))
				break;
		}
	}
	quit_request(QUIT_TIMER);
	stopping = true;
	for (std::thread &t : threads)
		t.join();
	camera::stop();
	restore_screen();
	log_msg("hello11 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

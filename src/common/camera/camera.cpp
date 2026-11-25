// camera.cpp - see camera.h.
#include "camera.h"
#include "types.h"

#include "bap.h"
#include "state.h"
#include "vcap.h"

extern "C" {
#include "ipc.h"
#include "log.h"
}

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace camera {

namespace {

constexpr unsigned kPollMs = 250;
// Every fifth telegram missing after this many: dev-ipc would be sharing
// them between readers, not copying them, and MMI3GApplication would miss
// ours. Stop reading.
constexpr u32 kGapCheckAfter = 20;

struct Sub {
	int id;
	const char *name;
	Handler h;
	Ch4Handler h4;
};

std::mutex mu;                          // the state below
Combiner comb;
VcapTracker tracker;                    // the slog thread, or start() before it
Ch4Stats stats4;

std::mutex smu;                         // subscribers; held while calling them
std::vector<Sub> subs;
int next_id = 1;

int slog_fd = -1;
std::atomic<bool> slog_quit{false};
std::thread slog_thread;
bool started = false;
int buffer[4096];                       // the biggest slog event is 258 ints

int ch4_sub = 0;                        // ipc.h id, 0: not subscribed
ipc4::Splitter splitter;                // ch4's reader thread only
std::vector<ipc4::Telegram> telegrams;  // the same

std::string who(const std::vector<Sub> &list, bool ch4)
{
	std::string s;
	char b[16];
	for (const Sub &x : list) {
		if ((x.h4 != nullptr) != ch4)
			continue;
		if (x.name == nullptr)
			std::snprintf(b, sizeof(b), "#%d", x.id);
		s += (s.empty() ? "" : ", ") + std::string(x.name ? x.name : b);
	}
	return s.empty() ? "nobody" : s;
}

// Hands a change of active() to the subscribers and logs it (after the
// state's lock: SD writes are slow).
void deliver(const Event &e)
{
	std::string to;
	{
		std::lock_guard<std::mutex> lock(smu);
		for (const Sub &s : subs)
			if (s.h)
				s.h(e);
		to = who(subs, false);
	}
	if (e.up)
		log_msg("camera: up (%s) -> %s", e.why, to.c_str());
	else
		log_msg("camera: off (%s), clear in %u ms -> %s", e.why, kSettleMs, to.c_str());
}

// ---- slog ----

// Everything new in the slog. false: read failed for good.
bool drain()
{
	for (;;) {
		isize n = read(slog_fd, buffer, sizeof(buffer));
		if (n < 0) {
			if (errno == EAGAIN)
				return true;
			if (errno == EINTR)
				continue;
			log_msg("camera: /dev/slog read: %s: no camera detection from now on",
			        std::strerror(errno));
			return false;
		}
		if (n == 0)
			return true;
		u32 now = log_ms();
		bool vcap_was, vcap_now, changed;
		bool up = false;
		{
			std::lock_guard<std::mutex> lock(mu);
			vcap_was = comb.vcap_owned();
			tracker.events(buffer, (usize)n / sizeof(int), now);
			vcap_now = tracker.active();
			changed = vcap_now != vcap_was && comb.vcap(vcap_now, now);
			up = comb.active();
		}
		if (vcap_now != vcap_was)
			log_msg("camera: VCAP 0 %s (slog)", vcap_now ? "taken by the HMI" : "free");
		if (changed)
			deliver(Event{up, "VCAP 0", now});
	}
}

void slog_run()
{
	while (!slog_quit) {
		if (!drain()) {
			std::lock_guard<std::mutex> lock(mu);
			comb = Combiner();
			return;
		}
		u32 now = log_ms();
		bool changed;
		{
			std::lock_guard<std::mutex> lock(mu);
			changed = comb.tick(now);
		}
		if (changed) {
			log_msg("camera: BAP said the camera starts, the HMI never took VCAP 0 in %u ms: "
			        "ignored", kBapAloneMs);
			deliver(Event{false, "BAP alone", now});
		}
		usleep(kPollMs * 1000);
	}
}

// ---- ch4 ----

void set_state(const char *s)
{
	std::lock_guard<std::mutex> lock(mu);
	stats4.state = s;
}

void bap_off(const char *why)
{
	bool changed;
	u32 now = log_ms();
	{
		std::lock_guard<std::mutex> lock(mu);
		changed = comb.bap(false, now);
	}
	if (changed)
		deliver(Event{false, why, now});
}

// One ch4 read, on ipc.h's ch4 reader thread.
void on_ch4(int, u32 now, const u8 *buf, int n, void *)
{
	ipc4::Splitter &sp = splitter;
	std::vector<ipc4::Telegram> &ts = telegrams;
	ts.clear();
	sp.feed(buf, (usize)n, ts);

	for (const ipc4::Telegram &t : ts) {
		Rvc r = rvc_state(t);
		if (r == Rvc::None)
			continue;
		bool on = r == Rvc::On, was, changed, up;
		{
			std::lock_guard<std::mutex> lock(mu);
			was = comb.bap_on();
			changed = comb.bap(on, now);
			up = comb.active();
		}
		if (on != was)
			log_msg("camera: BAP %02X.%02X session %s", kRvcLsg, kRvcSessionFid,
			        on ? "on" : "off");
		if (changed)
			deliver(Event{up, "BAP", now});
	}
	{
		std::lock_guard<std::mutex> lock(smu);
		for (const Sub &s : subs)
			if (s.h4)
				s.h4(now, buf, (usize)n, ts);
	}
	bool gaps = sp.telegrams() >= kGapCheckAfter && sp.gaps() * 5 > sp.telegrams();
	{
		std::lock_guard<std::mutex> lock(mu);
		stats4.telegrams = sp.telegrams();
		stats4.gaps = sp.gaps();
		stats4.missing = sp.missing();
		stats4.skipped = sp.skipped();
	}
	if (gaps) {
		log_msg("camera: ch4 STOP: %u gaps (%u telegrams missing) in %u telegrams: dev-ipc "
		        "shares them between readers, MMI3GApplication would miss ours",
		        sp.gaps(), sp.missing(), sp.telegrams());
		set_state("stopped: gaps");
		ipc_stop(4, "camera: gaps");
		bap_off("BAP gone");
	}
}

void start_ch4()
{
	if (ch4_sub > 0)
		return;
	splitter = ipc4::Splitter();
	ch4_sub = ipc_subscribe(4, on_ch4, nullptr, "camera");
	if (ch4_sub < 0) {
		log_msg("camera: no ch4 (%s): the slog alone", ipc_state(4));
		set_state(ipc_state(4));
		ch4_sub = 0;
		return;
	}
	set_state("reading");
}

void stop_ch4()
{
	if (ch4_sub <= 0)
		return;
	ipc_unsubscribe(ch4_sub);           // no on_ch4 runs after this
	ch4_sub = 0;
	set_state("stopped");
	bap_off("BAP gone");
}

int add(Sub s)
{
	std::lock_guard<std::mutex> lock(smu);
	s.id = next_id++;
	subs.push_back(s);
	return s.id;
}

void remove(int id)
{
	std::lock_guard<std::mutex> lock(smu);
	for (usize i = 0; i < subs.size(); i++)
		if (subs[i].id == id) {
			subs.erase(subs.begin() + (isize)i);
			return;
		}
}

} // namespace

void start(bool with_ch4)
{
	if (started)
		return;
	slog_fd = open("/dev/slog", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (slog_fd < 0) {
		log_msg("camera: /dev/slog: %s: no camera detection", std::strerror(errno));
		return;
	}
	if (!drain()) {
		close(slog_fd);
		slog_fd = -1;
		return;
	}
	log_msg("camera: slog read, the camera is %s", active() ? "UP" : "off");
	slog_quit = false;
	try {
		slog_thread = std::thread(slog_run);
	} catch (const std::system_error &) {
		log_msg("camera: no thread: no camera detection");
		std::lock_guard<std::mutex> lock(mu);
		comb = Combiner();
		close(slog_fd);
		slog_fd = -1;
		return;
	}
	started = true;
	if (with_ch4)
		start_ch4();
}

void stop()
{
	if (!started)
		return;
	slog_quit = true;
	slog_thread.join();
	close(slog_fd);
	slog_fd = -1;
	stop_ch4();
	started = false;
}

bool active()
{
	std::lock_guard<std::mutex> lock(mu);
	return comb.active();
}

bool clear()
{
	u32 now = log_ms();
	std::lock_guard<std::mutex> lock(mu);
	return comb.clear(now);
}

Status status()
{
	std::lock_guard<std::mutex> lock(mu);
	Status s;
	s.active = comb.active();
	s.vcap = comb.vcap_owned();
	s.bap = comb.bap_on();
	s.bap_unconfirmed = comb.unconfirmed();
	return s;
}

int subscribe(Handler h, const char *name)
{
	return add(Sub{0, name, std::move(h), nullptr});
}

void unsubscribe(int id)
{
	remove(id);
}

int subscribe_ch4(Ch4Handler h, const char *name)
{
	return add(Sub{0, name, nullptr, std::move(h)});
}

void unsubscribe_ch4(int id)
{
	remove(id);
}

Ch4Stats ch4_stats()
{
	std::lock_guard<std::mutex> lock(mu);
	return stats4;
}

} // namespace camera

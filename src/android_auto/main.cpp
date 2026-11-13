// android_auto - an Android Auto head unit on the MMI, proof of
// connectivity: a stock phone with the Android Auto app, plugged into the
// car's USB port, gets as far as streaming its projected screen to us.
//
//   phone (Android Auto)  --USB, AOA bulk-->  aoa_host  -->  session  -->  SD
//
// The phone's H.264 video is written to <sd>/var/android_auto.h264 (first
// save= MB) for a look on a PC; the audio channels are acked and counted,
// not played. Nothing of the projection is shown: the DM642 refuses 800-wide
// video (docs/android-auto.md). The screen is a CPU-drawn status page: a
// checklist of the connection steps (done / in progress / failed) with a
// hint for the current one, the details (phone, TLS, channels, frames,
// ping), how the previous session ended and the last lines of the log.
//
// Threads: io-usb's callback thread queues the bytes from bulk IN; the
// session thread runs aa::Session on them (TLS, protocol, acks, the video
// file); the USB manager thread (aoa_host) handles devices; main draws.
//
// Runs until the SD card is pulled or it gets SIGTERM/INT/HUP.
// Writes <sd>/var/android_auto.log (the previous run's: android_auto.prev.log).
//
// usage: android_auto <sd_path> [audio=0|1] [version=M.m] [save=MB] [tls=N] [fps=N]
//   audio=0     advertise no audio channels (default 1: media, guidance,
//               system; their PCM is counted and dropped)
//   fps=N       ack video frames at most N per second (default 25; 0 = at
//               once): the phone sends the next frame only after the ack
//   version=    AAP version we ask for (default 1.1)
//   save=MB     cap of android_auto.h264 (default 64, 0 = don't save)
//   tls=N       mbedTLS trace level in the log (default 1: errors)
#include "types.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include "aoa_host.h"
#include "camera.h"
#include "camera_log.h"
#include "mono_cond.h"
#include "session.h"

extern "C" {
#include "cpumon.h"
#include "font_big.h"
#include "font_small.h"
#include "log.h"
#include "quit.h"
#include "screen.h"
}

namespace {

constexpr unsigned kCpuWindowMs = 10000;
constexpr unsigned kCpuWarnPct = 50;
constexpr unsigned kStatsEveryMs = 10000;
constexpr unsigned kSessionTickMs = 50;
constexpr usize   kRxQueueMax = 8u << 20;   // bytes waiting for the session thread

// Identification for the AOA handshake: what the Android Auto app expects
// from a head unit (aasdk's AccessoryModeQueryFactory). Model
// "Android Auto" is what makes the phone start Android Auto.
const aoa_host::Config kUsb = {
	{"Android", "Android Auto", "Android Auto", "2.0.1", "https://github.com/dspl1236/MMI3G-Toolkit",
	 "MMI3G-AA-0001"},
	nullptr,
	kSessionTickMs,
};

struct Options {
	aa::Config cfg;
	unsigned   save_mb = 64;
	int        tls_debug = 1;
};

// --- what the screen shows (written by the other threads) -----------------

struct Status {
	aoa_host::State usb = aoa_host::State::NoStack;
	std::string     usb_detail = "starting";
	aa::Phase       phase = aa::Phase::Idle;
	aa::Phase       last_phase = aa::Phase::Idle;  // the last before Failed
	std::string     phase_detail;
	std::string     last_end;              // how the previous session ended
	aa::Stats       stats;
	unsigned        fps10 = 0;             // video frames/s x10 over the last window
	unsigned        kbps = 0;
	unsigned        held_pct = 0;          // video acks held back by pacing, % (last second)
	unsigned        wait_avg = 0;          // their average wait, ms
	u64        saved = 0;             // bytes in android_auto.h264
	unsigned        version = 0;           // bumped on every change
	char            pace_label[16] = "";   // "25 fps" or "off"
};

std::mutex status_lock;
Status     status;

template <class F>
void update(F &&f)
{
	std::lock_guard<std::mutex> lock(status_lock);
	f(status);
	status.version++;
}

Status snapshot()
{
	std::lock_guard<std::mutex> lock(status_lock);
	return status;
}

// The last log lines, for the screen (log_set_tap; must not log).
constexpr usize      kTailLines = 7;
std::mutex            tail_lock;
std::deque<std::string> tail;
std::atomic<unsigned> tail_version{0};

void tail_tap(const char *line)
{
	std::lock_guard<std::mutex> lock(tail_lock);
	tail.emplace_back(line);
	if (tail.size() > kTailLines)
		tail.pop_front();
	tail_version++;
}

std::deque<std::string> tail_snapshot()
{
	std::lock_guard<std::mutex> lock(tail_lock);
	return tail;
}

// --- the session thread ---------------------------------------------------

class App : public aoa_host::Client, public aa::Host {
public:
	App(const Options &o, const char *out_dir, std::string cert, std::string key)
		: opt_(o), out_(out_dir), cert_(std::move(cert)), key_(std::move(key)) {}

	bool start()
	{
		try {
			thread_ = std::thread([this] { run(); });
		} catch (const std::system_error &e) {
			log_msg("session thread: %s", e.what());
			return false;
		}
		return true;
	}
	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(lock_);
			stopping_ = true;
		}
		cond_.notify();
		if (thread_.joinable())
			thread_.join();
	}

	// aoa_host::Client (manager / usbd threads)
	void state(aoa_host::State s, const char *detail) override
	{
		update([&](Status &st) {
			st.usb = s;
			st.usb_detail = detail;
		});
	}
	void opened() override { post(Ev::Open); }
	void closed(const char *) override { post(Ev::Close); }
	void rx(const u8 *p, usize n) override
	{
		{
			std::lock_guard<std::mutex> lock(lock_);
			if (rx_bytes_ + n > kRxQueueMax) {
				if (!rx_overflow_)
					log_msg("session: rx queue over %zu bytes, dropping (session will fail)", kRxQueueMax);
				rx_overflow_ = true;
				return;
			}
			rx_.emplace_back(p, p + n);
			rx_bytes_ += n;
		}
		cond_.notify();
	}

	// aa::Host (session thread)
	void send(const u8 *p, usize n) override
	{
		if (!aoa_host::send(p, n))
			log_msg("session: %zu bytes not sent (no link / queue full)", n);
	}
	void phase(aa::Phase p, const std::string &detail) override
	{
		update([&](Status &st) {
			st.phase = p;
			if (p != aa::Phase::Failed)
				st.last_phase = p;
			st.phase_detail = detail;
		});
	}
	void video(const u8 *p, usize n, u64, bool) override
	{
		if (video_ == nullptr || video_saved_ + n > (u64)opt_.save_mb << 20)
			return;
		if (std::fwrite(p, 1, n, video_) == n)
			video_saved_ += n;
	}
	unsigned now_ms() override { return log_ms(); }

private:
	enum class Ev { Open, Close };

	void post(Ev e)
	{
		{
			std::lock_guard<std::mutex> lock(lock_);
			events_.push_back(e);
		}
		cond_.notify();
	}

	void open_session()
	{
		update([](Status &st) {
			st.stats = aa::Stats();
			st.last_phase = aa::Phase::Idle;
			st.fps10 = st.kbps = 0;
		});
		session_.reset(new aa::Session(opt_.cfg, *this));
		if (opt_.save_mb > 0 && video_ == nullptr) {
			char path[300];
			std::snprintf(path, sizeof(path), "%s/android_auto.h264", out_.c_str());
			video_ = std::fopen(path, "wb");
			if (video_ == nullptr)
				log_msg("session: cannot write %s", path);
		}
		session_->start(cert_, key_);
	}

	void close_session(const char *why)
	{
		if (!session_)
			return;
		const aa::Stats &s = session_->stats();
		log_msg("session: closed (%s) in phase %s: %llu video frames, %llu KB video, "
		        "%llu KB audio, %u pings sent / %u answered", why, aa::phase_name(session_->phase()),
		        (unsigned long long)s.video_frames, (unsigned long long)(s.video_bytes >> 10),
		        (unsigned long long)((s.audio_bytes[0] + s.audio_bytes[1] + s.audio_bytes[2]) >> 10),
		        s.pings_sent, s.pongs);
		unsigned long long frames = s.video_frames;
		std::string error = s.error;
		session_.reset();
		if (video_ != nullptr)
			std::fflush(video_);
		update([&](Status &st) {
			if (st.phase != aa::Phase::Failed)
				st.phase = aa::Phase::Idle;
			st.phase_detail = why;
			char end[200];
			std::snprintf(end, sizeof(end), "got to %s, %llu video frames - %s%s%s",
			              aa::phase_name(st.last_phase), frames, why, error.empty() ? "" : ": ",
			              error.c_str());
			st.last_end = end;
		});
	}

	void report(unsigned now)
	{
		const aa::Stats &s = session_->stats();
		unsigned dt = now - last_report_;
		u64 frames = s.video_frames - last_frames_, bytes = s.video_bytes - last_bytes_;
		u64 held = s.acks_held - last_held_, wait_sum = s.ack_wait_ms_sum - last_wait_sum_;
		unsigned fps10 = dt ? (unsigned)(frames * 10000 / dt) : 0;
		unsigned kbps = dt ? (unsigned)(bytes * 8 / dt) : 0;
		if (now - last_log_ >= kStatsEveryMs && session_->phase() == aa::Phase::Running) {
			last_log_ = now;
			log_msg("stats: video %llu frames, %u.%u fps, %u kbit/s, saved %llu KB; audio %llu/%llu/%llu KB; "
			        "rtt %u ms, pings %u/%u, phone's pings %u; out queue %zu B",
			        (unsigned long long)s.video_frames, fps10 / 10, fps10 % 10, kbps,
			        (unsigned long long)(video_saved_ >> 10),
			        (unsigned long long)(s.audio_bytes[0] >> 10), (unsigned long long)(s.audio_bytes[1] >> 10),
			        (unsigned long long)(s.audio_bytes[2] >> 10), s.last_rtt_ms, s.pongs, s.pings_sent,
			        s.pings_answered, aoa_host::out_pending());
			// pacing over the last window: acks held / frames, average and
			// max wait; frame age now (growing = queueing on the phone)
			log_msg("pace: %u fps cap, %llu of %llu acks held, avg %llu ms, max %u ms; frame age +%u ms "
			        "(max +%u ms)",
			        opt_.cfg.video_fps, (unsigned long long)(s.acks_held - log_held_),
			        (unsigned long long)(s.video_frames - log_frames_),
			        (unsigned long long)(s.acks_held > log_held_
			                                 ? (s.ack_wait_ms_sum - log_wait_sum_) / (s.acks_held - log_held_)
			                                 : 0),
			        s.ack_wait_ms_max, s.age_ms, s.age_ms_max);
			log_held_ = s.acks_held;
			log_frames_ = s.video_frames;
			log_wait_sum_ = s.ack_wait_ms_sum;
			if (video_ != nullptr)
				std::fflush(video_);
		}
		if (dt >= 1000) {
			last_report_ = now;
			last_frames_ = s.video_frames;
			last_bytes_ = s.video_bytes;
			last_held_ = s.acks_held;
			last_wait_sum_ = s.ack_wait_ms_sum;
		}
		update([&](Status &st) {
			st.stats = s;
			if (dt >= 1000) {
				st.fps10 = fps10;
				st.kbps = kbps;
				st.held_pct = frames ? (unsigned)(held * 100 / frames) : 0;
				st.wait_avg = held ? (unsigned)(wait_sum / held) : 0;
			}
			st.saved = video_saved_;
		});
	}

	void run()
	{
		cpumon_name_thread("session");
		std::deque<std::vector<u8>> rx;
		std::deque<Ev> events;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(lock_);
				// a held video ack may be due before the next tick
				unsigned wait = session_ ? session_->ms_to_next(kSessionTickMs) : kSessionTickMs;
				cond_.wait_ms(lock, wait,
				              [this] { return stopping_ || !rx_.empty() || !events_.empty(); });
				if (stopping_)
					break;
				rx.swap(rx_);
				events.swap(events_);
				rx_bytes_ = 0;
			}
			// open before its data (the phone says nothing before our
			// VERSION_REQUEST, so data never comes first), close after it
			for (Ev e : events)
				if (e == Ev::Open) {
					close_session("a new link");
					open_session();
				}
			if (session_)
				for (const std::vector<u8> &b : rx)
					session_->rx(b.data(), b.size());
			for (Ev e : events)
				if (e == Ev::Close)
					close_session("USB link closed");
			rx.clear();
			events.clear();
			if (session_) {
				session_->tick();
				report(log_ms());
			}
		}
		if (session_)
			session_->bye();
		close_session("stopping");
		if (video_ != nullptr)
			std::fclose(video_);
	}

	Options     opt_;
	std::string out_, cert_, key_;   // out_: <sd>/var
	std::thread thread_;

	std::mutex                       lock_;
	MonoCond                         cond_;
	bool                             stopping_ = false;
	std::deque<std::vector<u8>> rx_;
	usize                           rx_bytes_ = 0;
	bool                             rx_overflow_ = false;
	std::deque<Ev>                   events_;

	// session thread only
	std::unique_ptr<aa::Session> session_;
	FILE    *video_ = nullptr;
	u64 video_saved_ = 0;
	unsigned last_report_ = 0, last_log_ = 0;
	u64 last_frames_ = 0, last_bytes_ = 0, last_held_ = 0, last_wait_sum_ = 0;
	u64 log_held_ = 0, log_frames_ = 0, log_wait_sum_ = 0;
};

// --- the screen -------------------------------------------------------------

constexpr u32 kBg = 0xFF101820u;
constexpr u32 kText = 0xFFE8E8E8u;
constexpr u32 kDim = 0xFF8A929Eu;
constexpr int      kMargin = 16;

u32 bar_colour(const Status &s)
{
	if (s.phase == aa::Phase::Failed)
		return 0xFFA03028u;
	if (s.phase == aa::Phase::Running)
		return s.stats.video_started ? 0xFF2E8E4Au : 0xFF2F6FA8u;
	if (s.usb == aoa_host::State::Open || s.usb == aoa_host::State::Switching)
		return 0xFFB07A10u;
	return 0xFF3A4452u;
}

std::string title(const Status &s)
{
	if (s.phase == aa::Phase::Failed)
		return "Android Auto: failed";
	if (s.phase == aa::Phase::Running)
		return s.stats.video_started ? "Android Auto: streaming" : "Android Auto: connected";
	if (s.phase != aa::Phase::Idle)
		return std::string("Android Auto: ") + aa::phase_name(s.phase);
	if (s.usb == aoa_host::State::Switching)
		return "Phone found, switching...";
	if (s.usb == aoa_host::State::Open)
		return "Accessory open";
	return "Android Auto: waiting for phone";
}

// The connection steps, in order. done_steps() says how many are done.
struct Step {
	const char *name;
	const char *hint;      // what to do while it is the current step
	const char *fail_hint; // what to try when the session failed in it
};
const Step kSteps[] = {
	{"Phone on USB", "Plug the phone into the car's USB port (a data cable), unlock it",
	 "Check the head unit certificate in aa/ on the card (make sd)"},
	{"Accessory mode", "Unlock the phone and accept any Android Auto prompt on it",
	 "Unplug and replug the phone"},
	{"Protocol version", "Waiting for the phone's VERSION_RESPONSE",
	 "The phone refused the version: try version=1.6 in apps.conf"},
	{"TLS handshake", "Encrypting the link (a few seconds on this CPU)",
	 "TLS failed: the phone may refuse the certificate (log; tls=2 in apps.conf)"},
	{"Service discovery", "Waiting for the phone to ask for our services",
	 "Check the phone's screen for an Android Auto prompt"},
	{"Video channel open", "Waiting for the phone to open the video channel",
	 "The phone refused a service: see the log"},
	{"Video arriving", "Unlock the phone; video starts when Android Auto projects",
	 "See the log"},
};
constexpr int kNumSteps = sizeof(kSteps) / sizeof(kSteps[0]);

int done_steps(const Status &s)
{
	bool failed = s.phase == aa::Phase::Failed;
	aa::Phase p = failed ? s.last_phase : s.phase;
	if (p == aa::Phase::Idle)
		return s.usb == aoa_host::State::Open ? 2 : s.usb == aoa_host::State::Switching ? 1 : 0;
	int n = 2;                           // a session runs only on an open link
	if (p >= aa::Phase::Handshake)
		n = 3;
	if (p >= aa::Phase::Discovery)
		n = 4;
	if (p >= aa::Phase::Running)
		n = 5;
	if (n == 5 && (s.stats.open_channels & (1u << aa::kChVideo)))
		n = 6;
	if (n == 6 && s.stats.video_frames > 0)
		n = 7;
	return n;
}

void line(struct canvas *c, int x, int &y, u32 colour, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));
void line(struct canvas *c, int x, int &y, u32 colour, const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	text_draw(c, &font_small, x, y, buf, -1, colour);   // clipped at the screen's edge
	y += font_small.h + 6;
}

constexpr u32 kGood = 0xFF5CCB7Au;
constexpr u32 kBusy = 0xFFF0C040u;
constexpr u32 kBad = 0xFFFF8070u;

void render(struct canvas *c, const Status &s, const std::deque<std::string> &log_tail, unsigned step_s)
{
	canvas_fill(c, 0, 0, c->w, c->h, kBg);
	canvas_fill(c, 0, 0, c->w, 52, bar_colour(s));
	std::string t = title(s);
	text_draw(c, &font_big, kMargin, (52 - font_big.h) / 2, t.c_str(), -1, kText);

	// left: the steps
	const int pitch = font_small.h + 6;
	const int col2 = kMargin + 25 * font_small.w;
	bool failed = s.phase == aa::Phase::Failed;
	int done = done_steps(s);
	int y = 64;
	for (int i = 0; i < kNumSteps; i++) {
		const char *mark = "·";
		u32 colour = kDim;
		if (i < done) {
			mark = "√";
			colour = kGood;
		} else if (i == done) {
			mark = failed ? "✗" : "•";
			colour = failed ? kBad : kBusy;
		}
		text_draw(c, &font_small, kMargin, y, mark, -1, colour);
		text_draw(c, &font_small, kMargin + 2 * font_small.w, y, kSteps[i].name, -1,
		          i <= done ? kText : kDim);
		y += pitch;
	}
	const int steps_bottom = y;

	// right: the details
	const aa::Stats &st = s.stats;
	y = 64;
	line(c, col2, y, kText, "USB: %s - %s", aoa_host::state_name(s.usb), s.usb_detail.c_str());
	line(c, col2, y, kText, "Session: %s%s%s", aa::phase_name(s.phase), s.phase_detail.empty() ? "" : " - ",
	     s.phase_detail.c_str());
	if (!st.phone.empty())
		line(c, col2, y, kText, "Phone: %s", st.phone.c_str());
	if (!st.suite.empty())
		line(c, col2, y, kText, "TLS: %s (%u ms)", st.suite.c_str(), st.handshake_ms);
	if (st.open_channels != 0) {
		char ch[64] = "";
		for (int i = 1; i < 32; i++)
			if (st.open_channels & (1u << i))
				std::snprintf(ch + std::strlen(ch), sizeof(ch) - std::strlen(ch), " %d", i);
		line(c, col2, y, kText, "Channels open:%s", ch);
	}
	if (st.video_session >= 0 || st.video_frames > 0) {
		line(c, col2, y, kText, "Video: %llu frames, %u.%u fps, %u kbit/s, %llu KB saved",
		     (unsigned long long)st.video_frames, s.fps10 / 10, s.fps10 % 10, s.kbps,
		     (unsigned long long)(s.saved >> 10));
		line(c, col2, y, kText, "Pacing: %s, %u %% held (%u ms), age +%u ms", s.pace_label, s.held_pct,
		     s.wait_avg, st.age_ms);
	}
	if (st.audio_bytes[0] + st.audio_bytes[1] + st.audio_bytes[2] > 0)
		line(c, col2, y, kText, "Audio (dropped): %llu / %llu / %llu KB",
		     (unsigned long long)(st.audio_bytes[0] >> 10), (unsigned long long)(st.audio_bytes[1] >> 10),
		     (unsigned long long)(st.audio_bytes[2] >> 10));
	if (st.pings_sent > 0)
		line(c, col2, y, kText, "Ping: %u ms, %u of %u answered", st.last_rtt_ms, st.pongs, st.pings_sent);

	// below: error, hint, previous session, the log
	y = std::max(y, steps_bottom) + 4;
	if (!st.error.empty())
		line(c, kMargin, y, kBad, "Error: %s", st.error.c_str());
	if (done < kNumSteps) {
		const Step &step = kSteps[done];
		if (failed)
			line(c, kMargin, y, kBad, "Try: %s", step.fail_hint);
		else
			line(c, kMargin, y, kBusy, "Now: %s (%u s)", step.hint, step_s);
	}
	if (!s.last_end.empty())
		line(c, kMargin, y, kDim, "Previous session: %s", s.last_end.c_str());

	const int bottom = c->h - font_small.h - 8;
	int room = (bottom - y - 4) / pitch;
	if (room > 0 && !log_tail.empty()) {
		canvas_fill(c, kMargin, y, c->w - 2 * kMargin, 1, kDim);
		y += 4;
		usize first = log_tail.size() > (usize)room ? log_tail.size() - room : 0;
		for (usize i = first; i < log_tail.size(); i++) {
			const std::string &l = log_tail[i];
			bool bad = l.find("FAIL") != std::string::npos || l.find("fail") != std::string::npos ||
			           l.find("error") != std::string::npos;
			line(c, kMargin, y, bad ? kBad : kDim, "%s", l.c_str());
		}
	}

	int self = 0, unit = 0, high = 0;
	cpumon_now(&self, &unit, &high);
	y = bottom;
	line(c, kMargin, y, kDim, "CPU: android_auto %d %%, unit %d %%%s   %u s   pull the SD card to end", self,
	     unit, high ? " - HIGH" : "", log_ms() / 1000);
}

// --- start-up ----------------------------------------------------------------

void keep(const char *dir, const char *old_name, const char *new_name)
{
	char from[256], to[256];

	std::snprintf(from, sizeof(from), "%s/%s", dir, old_name);
	std::snprintf(to, sizeof(to), "%s/%s", dir, new_name);
	if (access(from, F_OK) != 0)
		return;
	unlink(to);
	rename(from, to);
}

std::string slurp(const std::string &path)
{
	std::string s;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return s;
	char buf[4096];
	usize n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	std::fclose(f);
	return s;
}

bool parse_args(int argc, char *argv[], Options &o)
{
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		unsigned m, n;
		if (std::strncmp(a, "audio=", 6) == 0)
			o.cfg.audio = a[6] != '0';
		else if (std::sscanf(a, "version=%u.%u", &m, &n) == 2) {
			o.cfg.version_major = (u16)m;
			o.cfg.version_minor = (u16)n;
		} else if (std::strncmp(a, "save=", 5) == 0)
			o.save_mb = (unsigned)std::atoi(a + 5);
		else if (std::strncmp(a, "tls=", 4) == 0)
			o.tls_debug = std::atoi(a + 4);
		else if (std::strncmp(a, "fps=", 4) == 0 && std::atoi(a + 4) >= 0 && std::atoi(a + 4) <= 60)
			o.cfg.video_fps = (unsigned)std::atoi(a + 4);
		else
			return false;
	}
	return true;
}

} // namespace

int main(int argc, char *argv[])
{
	char path[256];
	Options opt;

	if (argc < 2 || !parse_args(argc, argv, opt)) {
		std::fprintf(stderr, "usage: %s <sd_path> [audio=0|1] [version=M.m] [save=MB] [tls=N] [fps=N]\n",
		             argv[0]);
		return 2;
	}
	const char *sd_path = argv[1];
	const char *out_dir = var_dir(sd_path);
	keep(out_dir, "android_auto.log", "android_auto.prev.log");
	keep(out_dir, "android_auto.h264", "android_auto.prev.h264");
	std::snprintf(path, sizeof(path), "%s/android_auto.log", out_dir);
	if (log_open(path) != 0)
		std::fprintf(stderr, "android_auto: cannot open %s, logging to stdout only\n", path);
	log_set_tap(tail_tap);
	log_msg("android_auto start, pid=%d, protocol %u.%u, audio %s, save %u MB, tls trace %d, video acks %s%u%s",
	        getpid(), opt.cfg.version_major, opt.cfg.version_minor, opt.cfg.audio ? "on" : "off", opt.save_mb,
	        opt.tls_debug, opt.cfg.video_fps ? "paced to " : "at once", opt.cfg.video_fps,
	        opt.cfg.video_fps ? " fps" : "");
	quit_init(sd_path, 0);            // no watchdog: runs until SD pull / signal
	cpumon_name_thread("main");
	cpumon_start(kCpuWindowMs, kCpuWarnPct);
	aa::Tls::set_debug(opt.tls_debug);

	std::string cert = slurp(std::string(sd_path) + "/aa/headunit.crt");
	std::string key = slurp(std::string(sd_path) + "/aa/headunit.key");
	if (cert.empty() || key.empty())
		log_msg("FAIL: no head unit certificate in aa/ on the card (make sd puts it there)");
	else
		log_msg("head unit certificate: %zu + %zu bytes", cert.size(), key.size());

	camera::start();
	// our first show would cover the rear camera
	while (camera::active() && quit_poll() == QUIT_NONE)
		usleep(100 * 1000);
	bool screen_up = screen_open() == 0;
	if (!screen_up)
		log_msg("FAIL screen; running headless (log only)");
	screen_set_partial(1);
	camera::Takeover takeover;

	update([&](Status &st) {
		if (opt.cfg.video_fps)
			std::snprintf(st.pace_label, sizeof(st.pace_label), "%u fps", opt.cfg.video_fps);
		else
			std::snprintf(st.pace_label, sizeof(st.pace_label), "off");
	});
	App app(opt, out_dir, cert, key);
	aoa_host::Config usb = kUsb;
	usb.sd_path = sd_path;
	bool running = !cert.empty() && !key.empty() && app.start() && aoa_host::start(usb, app);
	if (!running)
		update([](Status &st) {
			st.phase = aa::Phase::Failed;
			st.stats.error = "no head unit certificate on the card (aa/headunit.crt, .key)";
		});

	unsigned last_version = ~0u, last_tail = ~0u, last_draw = 0;
	int last_done = -1;
	unsigned step_t0 = 0;
	while (quit_poll() == QUIT_NONE) {
		using Action = camera::Takeover::Action;
		switch (takeover.tick(screen_up, camera::active(), camera::clear(), log_ms())) {
		case Action::Retake:
			log_msg("screen: the rear camera has gone: back");
			screen_show_again();
			break;
		case Action::ShowAgain:
			screen_show_again();
			break;
		case Action::GiveBack:
			log_msg("screen: the rear camera started as the screen came back: the camera goes first");
			screen_give_back_to_camera();
			break;
		case Action::None:
			break;
		}
		Status s = snapshot();
		unsigned tv = tail_version.load();
		int done = done_steps(s);
		if (done != last_done) {
			last_done = done;
			step_t0 = log_ms();
		}
		if (screen_up && (s.version != last_version || tv != last_tail || log_ms() - last_draw >= 1000)) {
			last_version = s.version;
			last_tail = tv;
			last_draw = log_ms();
			render(screen_canvas(), s, tail_snapshot(), (log_ms() - step_t0) / 1000);
			screen_present();
		}
		usleep(100 * 1000);
	}

	app.stop();                       // says bye-bye while the link is up
	aoa_host::stop();
	if (screen_up) {
		if (camera::active())
			screen_no_show_on_close();
		screen_close();
	}
	camera::stop();
	cpumon_stop();
	log_set_tap(nullptr);
	log_msg("android_auto exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

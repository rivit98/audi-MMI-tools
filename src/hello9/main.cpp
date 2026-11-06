// hello9 - the unit's DM642 decoder showing a live H.264 stream, through the
// code audi_stream_receiver uses for the phone's frames (common/hw_stream.h:
// a CS-MP4 built on the MMI, a resource manager with blocking reads, gap
// fill with skip frames, the unit's own unpatched io-media, VCAP 0 input 1).
//
// 04.11 settled that a live stream plays (docs/phone-mirroring.md 7.10;
// that run's c9 / c10g / live / stall: src/hello9/README.md). Since then
// hello9 feeds a raw H.264 clip from the card (c11, Annex-B, as an encoder
// emits it) frame by frame, the way a phone would, and the MMI builds
// everything else. The driver turns the knob when the moving test picture
// is visible. Tests, in order:
//
//   c5      control: a plain MP4 (720x336) from the card, seen since 21.10
//   live    steady 25 fps, 30 s; the header declares the receiver's
//           default (~1.6 h)
//   gap     pauses of 2 s at 8 s and 5 s at 16 s, gap fill on: the
//           picture should freeze on the last frame before each pause
//           (199, 399) and go on without skipping (04.11 without fill: it
//           froze ~9 frames early and then skipped them)
//   slow    frames come at 15 fps; skip frames make up the rest of 25
//   burst   frames come 5 at a time every 200 ms (USB-like clumping)
//   s56     15 s steady, every frame padded to twice the size (56 KB)
//   s112    the same, 4x (112 KB)
//   f30     15 s, the stream timed at 30 fps (frames pushed at 30 fps)
//           s56 / s112 / f30 vs live: is io-media's ~0.5 s delay a queue
//           of bytes or of frames? (docs/phone-mirroring.md 7.10)
// and, on request only:
//   c9      constant-size MP4 file (seen 04.11)
//   stall   the gap test without gap fill (the 04.11 "stall")
//
// Every read io-media makes is counted (how far ahead it reads, how long
// it waits) and every TIME event gives the lag: how long after a frame was
// pushed io-media reports playing it.
//
// Output: <sd>/hello9.log (every step, RESULT lines, the summary) and
// <sd>/hello9/ (io-media queries, the system log after the tests).
//
// usage: hello9 <sd_path> [test,test,...]
//        (default: c5,live,gap,slow,burst,s56,s112,f30)
extern "C" {
#include "log.h"
#include "proc.h"
#include "quit.h"
#include "lmgr.h"
}

#include "csmp4.h"
#include "h264.h"
#include "hw_stream.h"
#include "player.h"
#include "schedule.h"
#include "ui.h"
#include "vout.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr unsigned kWatchdogS = 480;    // ~4.5 min run
constexpr unsigned kIntroMs = 12000;
constexpr unsigned kLabelMs = 7000;
constexpr unsigned kStartMs = 1000;     // play -> video displayable
constexpr unsigned kQueryMs = 2500;
constexpr unsigned kPageMs = 12000;
constexpr unsigned kFps = 25;
constexpr const char *kLivePath = "/dev/hello9/live.mp4";
constexpr const char *kRawClip = "c11_h264_336.h264";
constexpr const char *kFooter = "hello9 - streaming test. Pull the SD card to stop.";

enum class Kind { File, Live };

struct Test {
	const char *name;
	const char *title;
	const char *what;
	Kind kind;
	bool by_default;
	const char *clip;                 // File: the MP4 to play
	unsigned watch_ms;
	// Live: how frames come
	double fps;
	std::vector<plan::Stall> stalls;
	unsigned burst;
	bool fill;
	unsigned declared_s;              // 0: as many frames as fit (the receiver's default)
	unsigned s_mul = 1;               // frames padded to s_mul x the clip's S
	unsigned stream_fps = kFps;       // the rate the stream is timed at
};

const char kSeen[] = "\n\nTurn the main knob ONE click when you see the moving test picture "
                     "(its name and a counting number).";
const char kSeenStall[] =
	"\n\nTurn the main knob ONE click when you see the moving test picture, and ONE "
	"more click each time it starts moving again after it froze.";

const std::vector<Test> kTests = {
	{"c5", "Control: plain MP4", "The 720x336 video seen on every run since 21.10.", Kind::File,
	 true, "c5_h264_336.mp4", 8000, 0, {}, 1, false, 0},
	{"live", "Live stream (30 s)",
	 "hello9 sends the video frame by frame, like a phone would, and builds the stream "
	 "header itself.",
	 Kind::Live, true, nullptr, 30000, kFps, {}, 1, true, 0},
	{"gap", "Live stream with pauses (36 s)",
	 "The stream stops twice (2 s and 5 s), like a phone whose screen stands still. The "
	 "picture freezes: that is part of the test.",
	 Kind::Live, true, nullptr, 36000, kFps, {{200, 2000}, {400, 5000}}, 1, true, 600},
	{"slow", "Slow live stream (20 s)",
	 "Frames come at 15 a second instead of 25: the picture runs slower, but should not "
	 "stop.",
	 Kind::Live, true, nullptr, 20000, 15, {}, 1, true, 600},
	{"burst", "Live stream in bursts (20 s)",
	 "Frames come five at a time, five times a second.", Kind::Live, true, nullptr, 20000,
	 kFps, {}, 5, true, 600},
	{"s56", "Live stream, bigger frames (15 s)",
	 "The same stream, every frame padded to twice the size: does the delay change?",
	 Kind::Live, true, nullptr, 15000, kFps, {}, 1, true, 600, 2},
	{"s112", "Live stream, much bigger frames (15 s)",
	 "Every frame padded to four times the size.", Kind::Live, true, nullptr, 15000, kFps, {}, 1,
	 true, 600, 4},
	{"f30", "Live stream at 30 fps (15 s)",
	 "The stream timed at 30 frames a second: the counter runs a little fast.", Kind::Live, true,
	 nullptr, 15000, 30, {}, 1, true, 600, 1, 30},
	{"c9", "Constant-size MP4", "A file with every frame padded to the same size (seen 04.11).",
	 Kind::File, false, "c9_h264_336_cs.mp4", 8000, 0, {}, 1, false, 0},
	{"stall", "Live stream with pauses, no fill (36 s)",
	 "As the pause test, without the MMI filling the pauses. The picture freezes: that is "
	 "part of the test.",
	 Kind::Live, false, nullptr, 36000, kFps, {{200, 2000}, {400, 5000}}, 1, false, 600},
};

const char *sd_path;
char bin_dir[200];
std::mutex knob_mu;
std::vector<unsigned> knob_times;        // log_ms() of every ch8 message
std::vector<std::string> summary;

void on_key(const unsigned char *, int, void *)
{
	std::lock_guard<std::mutex> lock(knob_mu);
	knob_times.push_back(log_ms());
}

std::vector<unsigned> knobs_since(unsigned t0)
{
	std::lock_guard<std::mutex> lock(knob_mu);
	std::vector<unsigned> out;
	for (unsigned t : knob_times)
		if (t >= t0)
			out.push_back(t - t0);
	return out;
}

std::string out_path(const char *name)
{
	return std::string(sd_path) + "/hello9/" + name;
}

std::string clip_path(const char *clip)
{
	return std::string(sd_path) + "/clips/" + clip;
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

std::string tool_path(const char *name)
{
	static const char *const dirs[] = {"/mnt/efs-system/bin", "/mnt/efs-system/usr/bin",
	                                   "/usr/bin", "/bin"};
	for (const char *d : dirs) {
		std::string p = std::string(d) + "/" + name;
		if (access(p.c_str(), X_OK) == 0)
			return p;
	}
	std::string p = std::string(bin_dir) + "/" + name;
	return access(p.c_str(), X_OK) == 0 ? p : "";
}

// ---- the raw clip: frames as an encoder hands them out ----

struct Clip {
	std::vector<uint8_t> data;
	std::vector<h264::Range> frames;
	std::vector<uint8_t> sps, pps;
	uint32_t sample_size = 0;          // S: the biggest frame + filler, in 4 KB steps
};

std::string load_clip(const std::string &path, Clip &c)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return path + ": " + std::strerror(errno);
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	c.data.resize(size > 0 ? (size_t)size : 0);
	size_t got = std::fread(c.data.data(), 1, c.data.size(), f);
	std::fclose(f);
	if (got != c.data.size() || c.data.empty())
		return path + ": short read";
	c.frames = h264::split_access_units(c.data.data(), c.data.size());
	size_t biggest = 0;
	for (const h264::Range &r : c.frames) {
		size_t bytes = 0;
		for (const h264::Nal &n : h264::split_annexb(c.data.data() + r.begin, r.end - r.begin)) {
			if (n.type() == h264::kSps && c.sps.empty())
				c.sps.assign(n.p, n.p + n.n);
			if (n.type() == h264::kPps && c.pps.empty())
				c.pps.assign(n.p, n.p + n.n);
			if (n.type() != h264::kAud)
				bytes += 4 + n.n;
		}
		biggest = std::max(biggest, bytes);
	}
	if (c.sps.empty() || c.pps.empty() || c.frames.empty())
		return path + ": no SPS/PPS or no frames";
	c.sample_size = (uint32_t)((biggest + csmp4::kFillerMin + 4095) / 4096 * 4096);
	return "";
}

// Pushes the clip's frames (looped: frame 0 is an IDR) on a schedule, from
// its own thread, as the receiver's USB thread would.
class Feeder {
public:
	Feeder(HwStream &hw, const Clip &clip, const plan::Schedule &s) : hw_(hw), clip_(clip), s_(s) {}
	~Feeder() { stop(); }

	void start()
	{
		t0_ = log_ms();
		thread_ = std::thread([this] { run(); });
	}
	void stop()
	{
		stop_ = true;
		if (thread_.joinable())
			thread_.join();
	}
	uint64_t elapsed_ms() const { return log_ms() - t0_; }
	uint32_t pushed() const { return pushed_; }
	uint32_t refused() const { return refused_; }
	std::string first_refusal()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return why_;
	}

private:
	void run()
	{
		while (!stop_) {
			uint64_t due = s_.released(log_ms() - t0_);
			while (next_ < due && !stop_) {
				const h264::Range &r = clip_.frames[next_ % clip_.frames.size()];
				std::string why;
				live::Push p = hw_.push(clip_.data.data() + r.begin, r.end - r.begin, &why);
				if (p == live::Push::Ok) {
					pushed_++;
				} else {
					refused_++;
					std::lock_guard<std::mutex> lock(mu_);
					if (why_.empty())
						why_ = "frame " + std::to_string(next_) + ": " + live::push_name(p) +
						       (why.empty() ? "" : " (" + why + ")");
				}
				next_++;
			}
			usleep(2000);
		}
	}

	HwStream &hw_;
	const Clip &clip_;
	plan::Schedule s_;
	unsigned t0_ = 0;
	uint64_t next_ = 0;
	std::atomic<uint32_t> pushed_{0}, refused_{0};
	std::atomic<bool> stop_{false};
	std::thread thread_;
	std::mutex mu_;
	std::string why_;
};

// ---- io-media's own view of the graph (testapp-cmdline), in the background ----

struct Query {
	int video = -1;               // 1 video stream, 0 none, -1 unknown
	unsigned w = 0, h = 0;
};

Query query_graph(const std::string &graph, const std::string &log_name)
{
	Query q;
	std::string cmdline = tool_path("testapp-cmdline");
	if (cmdline.empty())
		return q;
	std::string script = out_path("iomquery.txt"), qlog = out_path(log_name.c_str());
	if (FILE *f = std::fopen(script.c_str(), "w")) {
		std::fputs("GetStatus\nGetTrackInfo\nGetVideoInfo\nquit\n", f);
		std::fclose(f);
	}
	char *argv[] = {(char *)cmdline.c_str(), (char *)"-a", (char *)graph.c_str(), (char *)"-O",
	                (char *)"-e", (char *)script.c_str(), nullptr};
	pid_t pid = proc_start(qlog.c_str(), argv);
	if (pid < 0)
		return q;
	int status = 0;
	if (proc_wait(pid, kQueryMs, &status) != 0)
		proc_stop(pid);
	if (FILE *f = std::fopen(qlog.c_str(), "r")) {
		char line[300];
		while (std::fgets(line, sizeof(line), f) != nullptr) {
			if (std::strstr(line, ", no video") != nullptr)
				q.video = 0;
			else if (std::strstr(line, ", video ('") != nullptr)
				q.video = 1;
			if (const char *p = std::strstr(line, "get_video_info() returned w="))
				std::sscanf(p, "get_video_info() returned w=%u h=%u", &q.w, &q.h);
		}
		std::fclose(f);
	}
	return q;
}

// ---- one test ----

struct Lags {
	std::vector<int64_t> v;
	int64_t last = 0;
	void add(int64_t x)
	{
		v.push_back(x);
		last = x;
	}
	std::string str() const
	{
		if (v.empty())
			return "none";
		std::vector<int64_t> s = v;
		std::sort(s.begin(), s.end());
		char b[96];
		std::snprintf(b, sizeof(b), "%lld/%lld/%lld/%lld ms", (long long)s.front(),
		              (long long)s[s.size() / 2], (long long)s.back(), (long long)last);
		return b;
	}
};

void log_notes(StreamServer &srv)
{
	for (const std::string &n : srv.take_notes())
		log_msg("  server: %s", n.c_str());
}

long long lead_max(const StreamStats &s)
{
	return s.lead_frames_max == INT64_MIN ? 0 : (long long)s.lead_frames_max;
}

struct Outcome {
	const PlayStats *ps = nullptr;
	Query q;
	bool video_on = false;
	unsigned t_play = 0, t_video = 0;
	Lags lags;
};

std::string knob_report(const Outcome &o, size_t *count)
{
	std::vector<unsigned> knobs = o.video_on ? knobs_since(o.t_video) : std::vector<unsigned>();
	std::string s;
	for (unsigned k : knobs) {
		char b[24];
		std::snprintf(b, sizeof(b), "%s%.1f", s.empty() ? "" : ",",
		              (k + (o.t_video - o.t_play)) / 1000.0);
		s += b;
	}
	*count = knobs.size();
	return s;
}

void result_line(const Test &t, const Outcome &o)
{
	const PlayStats &ps = *o.ps;
	size_t knobs = 0;
	std::string ks = knob_report(o, &knobs);
	log_msg("RESULT %s: playing_ms=%u first_time_ms=%u time_events=%u pos_ms=%lld errors=%u%s%s "
	        "eof=%d video=%d %ux%u knob=%zu at_s=[%s] lag=%s",
	        t.name, ps.playing_ms, ps.first_time_ms, ps.time_events, (long long)ps.max_pos_ms,
	        ps.error_events, ps.error_events ? " first: " : "", ps.first_error.c_str(),
	        ps.input_eof, o.q.video, o.q.w, o.q.h, knobs, ks.c_str(), o.lags.str().c_str());
	char line[160];
	std::snprintf(line, sizeof(line), "%-6s TIME %3u pos %5.1fs knob %zu%s%s", t.name,
	              ps.time_events, ps.max_pos_ms < 0 ? 0.0 : ps.max_pos_ms / 1000.0, knobs,
	              t.kind == Kind::Live ? "  lag " : "",
	              t.kind == Kind::Live ? o.lags.str().c_str() : "");
	summary.push_back(line);
}

// c5 / c9: an MP4 file on the card.
bool run_file(const Test &t, const std::string &graph)
{
	const std::string src = clip_path(t.clip);
	Player player;
	std::string err = player.open(graph, src);
	if (err.empty())
		err = player.play();
	Outcome o;
	o.t_play = log_ms();
	if (!err.empty()) {
		log_msg("FAIL %s: %s", t.name, err.c_str());
		summary.push_back(std::string(t.name) + ": " + err);
		player.close();
		return quit_poll() == QUIT_NONE;
	}
	auto on_time = [&](unsigned since_play, int64_t pos) { o.lags.add((int64_t)since_play - pos); };
	std::thread query;
	bool stop = false;
	while (!stop) {
		unsigned now = log_ms() - o.t_play;
		if (now >= kStartMs + t.watch_ms)
			break;
		if (!o.video_on && now >= kStartMs) {
			query = std::thread([&o, graph, &t] {
				o.q = query_graph(graph, std::string("query_") + t.name + ".log");
			});
			vout::show();
			o.video_on = true;
			o.t_video = log_ms();
		}
		player.poll(on_time);
		stop = sleep_quit(20);
	}
	if (o.video_on)
		vout::hide();
	player.poll(on_time);
	player.close();
	if (query.joinable())
		query.join();
	o.ps = &player.stats();
	result_line(t, o);
	return quit_poll() == QUIT_NONE;
}

// live / gap / slow / burst / stall: the clip pushed frame by frame
// through HwStream, as audi_stream_receiver pushes the phone's.
bool run_live(const Test &t, const std::string &graph, const Clip &clip)
{
	HwOptions o;
	o.cfg.sps = clip.sps;
	o.cfg.pps = clip.pps;
	o.cfg.fps = t.stream_fps;
	o.cfg.sample_size = clip.sample_size * t.s_mul;
	o.cfg.declared = t.declared_s * t.stream_fps;
	// frames kept for io-media: 8 MB, as audi_stream_receiver (hwvideo.cpp)
	o.cfg.ring_frames = std::min<uint32_t>(128, std::max<uint32_t>(16, (8u << 20) / o.cfg.sample_size));
	o.cfg.fill = t.fill;
	o.path = kLivePath;
	o.graph = graph;
	o.video_after_ms = kStartMs;

	HwStream hw;
	std::string err = hw.start(o);
	if (!err.empty()) {
		log_msg("FAIL %s: %s", t.name, err.c_str());
		summary.push_back(std::string(t.name) + ": " + err);
		return quit_poll() == QUIT_NONE;
	}
	plan::Schedule sched(t.fps, t.stalls, t.burst);
	log_msg("  feeder: %zu frames in the clip (looped), %.0f fps%s%s", clip.frames.size(), t.fps,
	        t.burst > 1 ? ", in bursts of " : "",
	        t.burst > 1 ? std::to_string(t.burst).c_str() : "");
	for (const plan::Stall &s : t.stalls)
		log_msg("  pause: %u ms before frame %u (%.1f s into the stream); the last frame "
		        "before it is %u",
		        s.ms, s.at_frame, s.at_frame / t.fps, s.at_frame - 1);
	Feeder feeder(hw, clip, sched);
	feeder.start();

	Outcome out;
	auto on_time = [&](unsigned, int64_t pos) {
		int64_t lag = hw.lag_ms(pos);
		if (lag != INT64_MIN)
			out.lags.add(lag);
	};
	std::thread query;
	unsigned last_stats = 0;
	bool stop = false;
	const unsigned t_feed = log_ms();
	while (!stop) {
		err = hw.poll(on_time);
		if (!err.empty()) {
			log_msg("FAIL %s: %s", t.name, err.c_str());
			break;
		}
		if (hw.video_on() && !out.video_on) {
			out.video_on = true;
			out.t_video = hw.video_ms();
			query = std::thread([&out, graph, &t] {
				out.q = query_graph(graph, std::string("query_") + t.name + ".log");
			});
		}
		log_notes(hw.server());
		unsigned now = log_ms() - t_feed;
		if (now - last_stats >= 1000) {
			last_stats = now;
			StreamStats s = hw.server().stats();
			live::Stats ls = hw.stream().stats();
			uint64_t off = hw.stream().data_off();
			long long served = s.served_end > off
			                       ? (long long)((s.served_end - off - 1) / hw.stream().sample_size())
			                       : -1;
			log_msg("  t=%5.1f s: pushed %u (+%u skips, %u gaps), served to frame %lld, %u reads "
			        "(%u waited, max %u ms), lead max %lld last %lld frames, %u waiting, "
			        "pos %lld ms, lag %lld ms",
			        now / 1000.0, ls.real, ls.skips, ls.gaps, served, s.reads, s.waited,
			        s.wait_ms_max, lead_max(s), (long long)s.lead_frames_last, s.waiting_now,
			        (long long)hw.player().stats().max_pos_ms, (long long)out.lags.last);
		}
		if (hw.playing() && log_ms() - hw.play_ms() >= kStartMs + t.watch_ms)
			break;
		if (!hw.playing() && now > 10000) {
			log_msg("FAIL %s: no graph after 10 s (first IDR never accepted?)", t.name);
			break;
		}
		stop = sleep_quit(20);
	}

	// teardown: frames stop, picture off, graph gone, server gone
	feeder.stop();
	out.t_play = hw.play_ms();
	hw.poll(on_time);
	hw.stop();
	if (query.joinable())
		query.join();
	log_notes(hw.server());

	StreamStats s = hw.server().stats();
	live::Stats ls = hw.stream().stats();
	out.ps = &hw.player().stats();
	result_line(t, out);
	log_msg("RESULT %s stream: opens=%u reads=%u preads=%u bytes=%llu read=%u..%u waited=%u "
	        "wait_max=%u ms wait_avg=%llu ms lead_max=%lld frames unblocked=%u far=%u eof=%u",
	        t.name, s.opens, s.reads, s.preads, (unsigned long long)s.bytes, s.min_read,
	        s.max_read, s.waited, s.wait_ms_max,
	        (unsigned long long)(s.waited ? s.wait_ms_sum / s.waited : 0), lead_max(s),
	        s.unblocked, s.far, s.eof);
	log_msg("RESULT %s frames: pushed=%u refused=%u%s%s in_stream=%u real=%u idr=%u skips=%u "
	        "gaps=%u too_big=%u wait_idr=%u bad=%u max_frame=%u sample=%u evicted_reads=%u",
	        t.name, feeder.pushed(), feeder.refused(), feeder.refused() ? " first: " : "",
	        feeder.first_refusal().c_str(), ls.frames, ls.real, ls.idr, ls.skips, ls.gaps,
	        ls.too_big, ls.wait_idr, ls.bad, ls.max_frame, hw.stream().sample_size(),
	        ls.evicted_reads);
	char line[160];
	std::snprintf(line, sizeof(line), "       reads %u, waited %u (max %u ms), skips %u, refused %u",
	              s.reads, s.waited, s.wait_ms_max, ls.skips, feeder.refused());
	summary.push_back(line);
	return quit_poll() == QUIT_NONE;
}

// Run one test; false if hello9 should stop.
bool run_test(int n, int total, const Test &t, const Clip *clip, const std::string &clip_err)
{
	static unsigned graphs;
	char title[96];
	std::snprintf(title, sizeof(title), "Test %d/%d: %s", n, total, t.title);
	log_msg("== test %d/%d %s: %s (%s)", n, total, t.name, t.title,
	        t.kind == Kind::File ? t.clip : kRawClip);

	if (t.kind == Kind::File && access(clip_path(t.clip).c_str(), R_OK) != 0) {
		log_msg("FAIL %s: %s missing", t.name, clip_path(t.clip).c_str());
		summary.push_back(std::string(t.name) + ": clip missing");
		return true;
	}
	if (t.kind == Kind::Live && clip == nullptr) {
		log_msg("FAIL %s: %s", t.name, clip_err.c_str());
		summary.push_back(std::string(t.name) + ": " + clip_err);
		return true;
	}

	ui::show(title, std::string(t.what) + (t.stalls.empty() ? kSeen : kSeenStall), kFooter);
	if (sleep_quit(kLabelMs))
		return false;
	ui::hide();

	char gname[48];
	std::snprintf(gname, sizeof(gname), "hello9_%d_%u", (int)getpid(), ++graphs);
	return t.kind == Kind::File ? run_file(t, gname) : run_live(t, gname, *clip);
}

bool selected(const char *only, const Test &t)
{
	if (only == nullptr)
		return t.by_default;
	std::string list = std::string(",") + only + ",";
	return list.find(std::string(",") + t.name + ",") != std::string::npos;
}

// Leave no displayable of ours behind; the screen back to the HMI.
void restore_screen()
{
	int cids[3] = {40, 16, 19};
	ui::hide();
	if (lm_open() != 0)
		return;
	lm_unregister_displayable(41);
	lm_unregister_displayable(40);
	log_msg("restore: lm_show_displayables(0, {40,16,19}): rc=%d", lm_show_displayables(0, cids, 3));
	lm_close();
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path> [test,test,...]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	const char *only = argc > 2 && argv[2][0] ? argv[2] : nullptr;
	std::snprintf(bin_dir, sizeof(bin_dir), "%s", argv[0]);
	if (char *slash = std::strrchr(bin_dir, '/'))
		*slash = 0;
	else
		std::snprintf(bin_dir, sizeof(bin_dir), ".");

	mkdir(out_path("").c_str(), 0777);
	std::string logpath = std::string(sd_path) + "/hello9.log";
	if (log_open(logpath.c_str()) != 0)
		std::fprintf(stderr, "hello9: cannot open %s, logging to stdout only\n", logpath.c_str());
	log_msg("hello9 start, pid=%d%s%s", (int)getpid(), only ? ", only: " : "", only ? only : "");
	quit_init_ex(sd_path, kWatchdogS, on_key, nullptr);
	log_msg("testapp-cmdline: %s", tool_path("testapp-cmdline").c_str());

	std::vector<const Test *> run;
	bool live = false;
	for (const Test &t : kTests)
		if (selected(only, t)) {
			run.push_back(&t);
			live = live || t.kind == Kind::Live;
		}

	Clip clip;
	std::string clip_err = live ? load_clip(clip_path(kRawClip), clip) : "";
	if (live) {
		if (clip_err.empty()) {
			h264::Sps s;
			h264::parse_sps(clip.sps.data(), clip.sps.size(), s);
			log_msg("clip %s: %zu bytes, %zu frames, %ux%u, SPS %zu / PPS %zu bytes, POC type "
			        "%u, sample size %u",
			        kRawClip, clip.data.size(), clip.frames.size(), s.width, s.height,
			        clip.sps.size(), clip.pps.size(), s.poc_type, clip.sample_size);
		} else {
			log_msg("FAIL clip: %s", clip_err.c_str());
		}
	}

	char intro[600];
	std::snprintf(intro, sizeof(intro),
	              "Park the car. %zu test videos, one after the other, with a screen before "
	              "each. Turn the main knob ONE click whenever you see the moving test picture. "
	              "Most run for 15-36 s; in the pause test the picture freezes twice, which is "
	              "part of the test.\n\nLeave the card in until the results screen has gone.",
	              run.size());
	ui::show("hello9: streaming test (~4.5 min)", intro, kFooter);
	if (!sleep_quit(kIntroMs)) {
		int n = 0;
		for (const Test *t : run)
			if (!run_test(++n, (int)run.size(), *t, clip_err.empty() ? &clip : nullptr, clip_err))
				break;
	}
	vout::close_all();
	log_msg("== system log after the tests -> %s: %s", out_path("slog.txt").c_str(),
	        proc_status_str(proc_shell(out_path("slog.txt").c_str(), 30, "sloginfo")));

	log_msg("== summary (lag = min/median/max/last ms after the frame was pushed; knob = turns seen)");
	for (const std::string &s : summary)
		log_msg("  %s", s.c_str());
	for (size_t i = 0; i < summary.size() && quit_poll() == QUIT_NONE;) {
		std::vector<std::string> page(summary.begin() + (long)i, summary.end());
		int dropped = ui::show_list("hello9 results (also in hello9.log)", page);
		size_t shown = page.size() - (size_t)dropped;
		if (shown == 0)
			break;
		i += shown;
		sleep_quit(kPageMs);
	}
	if (summary.empty() && quit_poll() == QUIT_NONE) {
		ui::show_list("hello9 results", {"no test ran"});
		sleep_quit(kPageMs);
	}
	quit_request(QUIT_TIMER);
	restore_screen();
	log_msg("hello9 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

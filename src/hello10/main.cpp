// hello10 - can the DM642 show an 800-wide H.264 stream cropped to 720 in
// its SPS? Android Auto's smallest video is 800x480; io-media refuses
// anything wider than 720 (docs/android-auto.md 4.1). With a width margin
// of 80 px the phone keeps its UI in the left 720 columns, and the MMI
// rewrites the SPS (h264::crop_sps) so the stream says 720x480, Level 3.0.
// The coded picture stays 800 wide (50 macroblocks): whether the DSP
// decodes it, trips its width assertion or garbles it is what hello10
// answers.
//
// The clip, c12 (media/mkclips.sh), is raw 800x480 Baseline Level 3.1 as
// the phone sends it: the moving picture (label, counter) in the left 720
// columns, a red "CUT" band in the right 80, a blue band in the bottom 64
// lines. hello10 rewrites every SPS in it and pushes it frame by frame
// through HwStream, the code audi_stream_receiver uses (CS-MP4, resource
// manager, gap fill, VCAP 0 input 1). Tests, in order:
//
//   c5       control: a plain MP4 (720x336) from the card, plays every run
//   crop     800x480 cropped to 720x480, 25 fps, 30 s. Red band visible =
//            the DSP ignores the crop
//   cropnav  cropped to 720x416, 20 s: the blue band gone too, the picture
//            stretched to the full height? (cutting Android Auto's bar)
//   slow     720x480 crop, frames at 15 fps, gap fill to 25, 20 s: skip
//            frames on an 800-wide coded picture (a paced phone)
//
// The driver turns the knob once when the moving picture appears and films
// the screen (the bands are only on video).
//
// Output: <sd>/var/hello10.log (every step, RESULT lines, the summary) and
// <sd>/var/hello10/ (io-media queries, the system log after the tests).
//
// usage: hello10 <sd_path> [test,test,...]   (default: c5,crop,cropnav,slow)
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

constexpr unsigned kWatchdogS = 300;    // ~2.5 min run
constexpr unsigned kIntroMs = 12000;
constexpr unsigned kLabelMs = 7000;
constexpr unsigned kStartMs = 1000;     // play -> video displayable
constexpr unsigned kQueryMs = 2500;
constexpr unsigned kPageMs = 12000;
constexpr unsigned kFps = 25;
constexpr uint8_t kLevel = 30;          // what the stream says after the rewrite
constexpr const char *kLivePath = "/dev/hello10/live.mp4";
constexpr const char *kRawClip = "c12_h264_800.h264";
constexpr const char *kFooter = "hello10 - 800 wide cropped to 720. Pull the SD card to stop.";

enum class Kind { File, Live };

struct Test {
	const char *name;
	const char *title;
	const char *what;
	Kind kind;
	const char *clip;                 // File: the MP4 to play
	unsigned watch_ms;
	double fps;                       // Live: the rate frames come at
	unsigned crop_w, crop_h;          // Live: what the SPS says after the rewrite
};

const char kSeen[] = "\n\nFilm the screen. Turn the main knob ONE click when you see the moving "
                     "test picture (its name and a counting number).";

const std::vector<Test> kTests = {
	{"c5", "Control: plain MP4", "The 720x336 video seen on every run.", Kind::File,
	 "c5_h264_336.mp4", 8000, 0, 0, 0},
	{"crop", "800 wide, cropped to 720 (30 s)",
	 "An 800x480 stream that says 720x480. Is there a RED band with \"CUT\" on the right "
	 "edge? It should not be there.",
	 Kind::Live, nullptr, 30000, kFps, 720, 480},
	{"cropnav", "Cropped to 720x416 (20 s)",
	 "The same, with the bottom 64 lines cut too. Is the BLUE band at the bottom gone, and "
	 "does the picture fill the screen's height?",
	 Kind::Live, nullptr, 20000, kFps, 720, 416},
	{"slow", "Cropped, 15 frames a second (20 s)",
	 "As the first, with frames coming at 15 a second: the counter runs slower, but should "
	 "not stop or break up.",
	 Kind::Live, nullptr, 20000, 15, 720, 480},
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
	return std::string(var_dir(sd_path)) + "/hello10/" + name;
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

// ---- the raw clip, as the phone would send it, and cropped ----

struct Clip {
	std::vector<uint8_t> data;
	std::vector<h264::Range> frames;
	std::vector<uint8_t> sps, pps;
	uint32_t sample_size = 0;          // S: the biggest frame + filler, in 4 KB steps
};

std::string read_file(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return path + ": " + std::strerror(errno);
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out.resize(size > 0 ? (size_t)size : 0);
	size_t got = std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	if (got != out.size() || out.empty())
		return path + ": short read";
	return "";
}

// Split into frames; SPS / PPS and S from them.
std::string index_clip(Clip &c)
{
	c.frames = h264::split_access_units(c.data.data(), c.data.size());
	c.sps.clear();
	c.pps.clear();
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
		return "no SPS/PPS or no frames";
	c.sample_size = (uint32_t)((biggest + csmp4::kFillerMin + 4095) / 4096 * 4096);
	return "";
}

// The clip with every SPS rewritten: shows w x h, says kLevel.
std::string crop_clip(const Clip &in, unsigned w, unsigned h, Clip &out)
{
	out = Clip();
	std::vector<uint8_t> au;
	for (const h264::Range &r : in.frames) {
		std::string err = h264::crop_sps_annexb(in.data.data() + r.begin, r.end - r.begin, w, h,
		                                        kLevel, au);
		if (!err.empty())
			return err;
		out.data.insert(out.data.end(), au.begin(), au.end());
	}
	return index_clip(out);
}

std::string sps_str(const std::vector<uint8_t> &sps)
{
	h264::Sps s;
	std::string err = h264::parse_sps(sps.data(), sps.size(), s);
	if (!err.empty())
		return err;
	char b[120];
	std::snprintf(b, sizeof(b), "%ux%u coded %ux%u, profile %u level %u.%u, POC type %u, %zu bytes",
	              s.width, s.height, s.width_mbs * 16, s.height_mbs * 16, s.profile, s.level / 10,
	              s.level % 10, s.poc_type, sps.size());
	return b;
}

// Pushes the clip's frames (looped: frame 0 is an IDR) at a steady rate,
// from its own thread, as the receiver's USB thread would.
class Feeder {
public:
	Feeder(HwStream &hw, const Clip &clip, double fps) : hw_(hw), clip_(clip), fps_(fps) {}
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
			// frame k is due at k / fps
			uint64_t due = (uint64_t)((double)(log_ms() - t0_) * fps_ / 1000.0) + 1;
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
	double fps_;
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

struct Outcome {
	const PlayStats *ps = nullptr;
	Query q;
	bool video_on = false;
	unsigned t_play = 0, t_video = 0;
	Lags lags;
};

void result_line(const Test &t, const Outcome &o)
{
	const PlayStats &ps = *o.ps;
	std::vector<unsigned> knobs = o.video_on ? knobs_since(o.t_video) : std::vector<unsigned>();
	std::string ks;
	for (unsigned k : knobs) {
		char b[24];
		std::snprintf(b, sizeof(b), "%s%.1f", ks.empty() ? "" : ",",
		              (k + (o.t_video - o.t_play)) / 1000.0);
		ks += b;
	}
	log_msg("RESULT %s: playing_ms=%u first_time_ms=%u time_events=%u pos_ms=%lld errors=%u%s%s "
	        "eof=%d video=%d %ux%u knob=%zu at_s=[%s] lag=%s",
	        t.name, ps.playing_ms, ps.first_time_ms, ps.time_events, (long long)ps.max_pos_ms,
	        ps.error_events, ps.error_events ? " first: " : "", ps.first_error.c_str(),
	        ps.input_eof, o.q.video, o.q.w, o.q.h, knobs.size(), ks.c_str(), o.lags.str().c_str());
	char line[160];
	std::snprintf(line, sizeof(line), "%-7s TIME %3u pos %5.1fs knob %zu%s%s%s", t.name,
	              ps.time_events, ps.max_pos_ms < 0 ? 0.0 : ps.max_pos_ms / 1000.0, knobs.size(),
	              ps.error_events ? " ERRORS" : "", t.kind == Kind::Live ? "  lag " : "",
	              t.kind == Kind::Live ? o.lags.str().c_str() : "");
	summary.push_back(line);
}

// c5: an MP4 file on the card.
bool run_file(const Test &t, const std::string &graph)
{
	Player player;
	std::string err = player.open(graph, clip_path(t.clip));
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

// crop / cropnav / slow: the cropped clip pushed frame by frame through
// HwStream, as audi_stream_receiver pushes the phone's.
bool run_live(const Test &t, const std::string &graph, const Clip &raw)
{
	Clip clip;
	std::string err = crop_clip(raw, t.crop_w, t.crop_h, clip);
	if (err.empty()) {
		h264::Sps s;
		h264::parse_sps(clip.sps.data(), clip.sps.size(), s);
		err = live::check_decodable(s, kFps);
	}
	if (!err.empty()) {
		log_msg("FAIL %s: crop to %ux%u: %s", t.name, t.crop_w, t.crop_h, err.c_str());
		summary.push_back(std::string(t.name) + ": " + err);
		return quit_poll() == QUIT_NONE;
	}
	log_msg("  SPS after the rewrite: %s; sample size %u", sps_str(clip.sps).c_str(),
	        clip.sample_size);

	HwOptions o;
	o.cfg.sps = clip.sps;
	o.cfg.pps = clip.pps;
	o.cfg.fps = kFps;
	o.cfg.sample_size = clip.sample_size;
	o.cfg.declared = 600 * kFps;
	// frames kept for io-media: 8 MB, as audi_stream_receiver (hwvideo.cpp)
	o.cfg.ring_frames = std::min<uint32_t>(128, std::max<uint32_t>(16, (8u << 20) / o.cfg.sample_size));
	o.cfg.fill = true;
	o.path = kLivePath;
	o.graph = graph;
	o.video_after_ms = kStartMs;

	HwStream hw;
	err = hw.start(o);
	if (!err.empty()) {
		log_msg("FAIL %s: %s", t.name, err.c_str());
		summary.push_back(std::string(t.name) + ": " + err);
		return quit_poll() == QUIT_NONE;
	}
	log_msg("  feeder: %zu frames in the clip (looped), %.0f fps", clip.frames.size(), t.fps);
	Feeder feeder(hw, clip, t.fps);
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
			summary.push_back(std::string(t.name) + ": " + err);
			break;
		}
		if (hw.video_on() && !out.video_on) {
			out.video_on = true;
			out.t_video = hw.video_ms();
			query = std::thread([&out, graph, &t] {
				out.q = query_graph(graph, std::string("query_") + t.name + ".log");
			});
		}
		for (const std::string &n : hw.server().take_notes())
			log_msg("  server: %s", n.c_str());
		unsigned now = log_ms() - t_feed;
		if (now - last_stats >= 1000) {
			last_stats = now;
			StreamStats s = hw.server().stats();
			live::Stats ls = hw.stream().stats();
			log_msg("  t=%5.1f s: pushed %u (+%u skips), %u reads (%u waited, max %u ms), "
			        "%u waiting, pos %lld ms, TIME %u, errors %u, lag %lld ms",
			        now / 1000.0, ls.real, ls.skips, s.reads, s.waited, s.wait_ms_max,
			        s.waiting_now, (long long)hw.player().stats().max_pos_ms,
			        hw.player().stats().time_events, hw.player().stats().error_events,
			        (long long)out.lags.last);
		}
		if (hw.playing() && log_ms() - hw.play_ms() >= kStartMs + t.watch_ms)
			break;
		if (!hw.playing() && now > 10000) {
			log_msg("FAIL %s: no graph after 10 s (first IDR never accepted?)", t.name);
			break;
		}
		stop = sleep_quit(20);
	}

	feeder.stop();
	out.t_play = hw.play_ms();
	hw.poll(on_time);
	hw.stop();
	if (query.joinable())
		query.join();
	for (const std::string &n : hw.server().take_notes())
		log_msg("  server: %s", n.c_str());

	StreamStats s = hw.server().stats();
	live::Stats ls = hw.stream().stats();
	out.ps = &hw.player().stats();
	result_line(t, out);
	log_msg("RESULT %s stream: reads=%u bytes=%llu waited=%u wait_max=%u ms unblocked=%u far=%u",
	        t.name, s.reads, (unsigned long long)s.bytes, s.waited, s.wait_ms_max, s.unblocked, s.far);
	log_msg("RESULT %s frames: pushed=%u refused=%u%s%s real=%u idr=%u skips=%u too_big=%u "
	        "bad=%u max_frame=%u sample=%u",
	        t.name, feeder.pushed(), feeder.refused(), feeder.refused() ? " first: " : "",
	        feeder.first_refusal().c_str(), ls.real, ls.idr, ls.skips, ls.too_big, ls.bad,
	        ls.max_frame, hw.stream().sample_size());
	return quit_poll() == QUIT_NONE;
}

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

	ui::show(title, std::string(t.what) + kSeen, kFooter);
	if (sleep_quit(kLabelMs))
		return false;
	ui::hide();

	char gname[48];
	std::snprintf(gname, sizeof(gname), "hello10_%d_%u", (int)getpid(), ++graphs);
	return t.kind == Kind::File ? run_file(t, gname) : run_live(t, gname, *clip);
}

bool selected(const char *only, const Test &t)
{
	if (only == nullptr)
		return true;
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
	std::string logpath = std::string(var_dir(sd_path)) + "/hello10.log";
	if (log_open(logpath.c_str()) != 0)
		std::fprintf(stderr, "hello10: cannot open %s, logging to stdout only\n", logpath.c_str());
	log_msg("hello10 start, pid=%d%s%s", (int)getpid(), only ? ", only: " : "", only ? only : "");
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
	std::string clip_err;
	if (live) {
		clip_err = read_file(clip_path(kRawClip), clip.data);
		if (clip_err.empty())
			clip_err = index_clip(clip);
		if (clip_err.empty())
			log_msg("clip %s: %zu bytes, %zu frames, SPS as sent: %s", kRawClip, clip.data.size(),
			        clip.frames.size(), sps_str(clip.sps).c_str());
		else
			log_msg("FAIL clip: %s", clip_err.c_str());
	}

	char intro[600];
	std::snprintf(intro, sizeof(intro),
	              "Park the car and film the screen. %zu test videos, one after the other, with "
	              "a screen before each. Turn the main knob ONE click whenever you see the "
	              "moving test picture.\n\nIf the radio or the screen stops working afterwards, "
	              "restart the MMI (MENU + main knob + top-right key).",
	              run.size());
	ui::show("hello10: 800 wide, cropped to 720 (~2.5 min)", intro, kFooter);
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
	if (quit_poll() == QUIT_NONE) {
		ui::show_list("hello10 results (also in hello10.log)",
		              summary.empty() ? std::vector<std::string>{"no test ran"} : summary);
		sleep_quit(kPageMs);
	}
	quit_request(QUIT_TIMER);
	restore_screen();
	log_msg("hello10 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

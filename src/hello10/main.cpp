// hello10 - can io-media capture an 800-sample line? io-media-nvidia
// captures the DSP's video signal at a literal 720 wide (three 16-bit
// words, docs/phone-mirroring.md 7.14). Writing them in the running process
// is refused (ESRVRFAULT), so hello10 runs a second io-media from RAM next
// to the HMI's own (iomcopy.h) and plays c3 (720x480) three times:
//
//   copy720  an unchanged copy: can a second io-media run at all?
//   copy800  only if copy720 played: the copy with the width words at 800.
//            Right edge black or frozen = the capture stops at the line's
//            end (800 possible); garbled or slanted = it counts pixels
//   orig720  the HMI's io-media again: still fine?
//
// probe (read only): io-media's width words through
// /proc/<pid>/as, its arguments and environment, its segment list
// (capture::maps) and the physical address of the width page as our own
// shared mapping of the file sees it (capture::phys).
//
// The driver turns the knob once when the moving picture appears and films
// the screen.
//
// Output: <sd>/var/hello10.log (every step, RESULT lines, the summary) and
// <sd>/var/hello10/ (io-media queries, the copy's output, the system log).
//
// usage: hello10 <sd_path> [test,test,...]   (default: all)
#include "types.h"

extern "C" {
#include "iom.h"
#include "log.h"
#include "proc.h"
#include "ipc.h"
#include "lmgr.h"
}
#include "quit.h"

#include "capture.h"
#include "iomcopy.h"
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

constexpr unsigned kWatchdogS = 480;    // the default run (every test) is ~3 min
constexpr unsigned kIntroMs = 12000;
constexpr unsigned kLabelMs = 7000;
constexpr unsigned kStartMs = 1000;     // play -> video displayable
constexpr unsigned kQueryMs = 2500;
constexpr unsigned kPageMs = 12000;
constexpr const char *kFooter = "hello10 - capture 800 wide. Pull the SD card to stop.";

enum class Kind { File, Probe, Copy };

struct Test {
	const char *name;
	const char *title;
	const char *what;
	Kind kind;
	const char *clip;                 // the MP4 to play
	unsigned watch_ms;
};

const char kSeen[] = "\n\nFilm the screen. Turn the main knob ONE click when you see the moving "
                     "test picture (its name and a counting number).";

const std::vector<Test> kTests = {
	{"iomcopy", "A second io-media (about 2 min)",
	 "A copy of the MMI's media service starts next to the original, plays a video, then a "
	 "copy that captures 800 wide plays it, then the original plays it again. If the radio "
	 "goes quiet or the screen stops: note when, then restart the MMI (MENU + main knob + "
	 "top-right key).",
	 Kind::Copy, "c3_h264_480.mp4", 0},
	{"probe", "Probe io-media's capture width (no change)",
	 "Reads io-media's width words, how its code is mapped and where that page sits. "
	 "Nothing changes; nothing to do.",
	 Kind::Probe, nullptr, 0},
};

const char *sd_path;
char bin_dir[200];
std::mutex knob_mu;
std::vector<unsigned> knob_times;        // log_ms() of every ch8 message
std::vector<std::string> summary;

void on_key(int, u32 ms, const u8 *, int, void *)
{
	std::lock_guard<std::mutex> lock(knob_mu);
	knob_times.push_back(ms);
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
	while (!quit::stopping()) {
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
	std::vector<i64> v;
	i64 last = 0;
	void add(i64 x)
	{
		v.push_back(x);
		last = x;
	}
	std::string str() const
	{
		if (v.empty())
			return "none";
		std::vector<i64> s = v;
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
	              ps.error_events ? " ERRORS" : "", "  lag ", o.lags.str().c_str());
	summary.push_back(line);
}

// An MP4 file on the card. *time_events: the TIME events seen (did it play).
bool run_file(const Test &t, const std::string &graph, unsigned *time_events = nullptr)
{
	// testapp-cmdline only knows /dev/io-media: no query of another instance
	const bool own_iom = std::strcmp(iom_root(), "/dev/io-media") == 0;
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
		return !quit::stopping();
	}
	auto on_time = [&](unsigned since_play, i64 pos) { o.lags.add((i64)since_play - pos); };
	std::thread query;
	bool stop = false;
	while (!stop) {
		unsigned now = log_ms() - o.t_play;
		if (now >= kStartMs + t.watch_ms)
			break;
		if (!o.video_on && now >= kStartMs) {
			if (own_iom)
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
	if (time_events != nullptr)
		*time_events = player.stats().time_events;
	return !quit::stopping();
}

// probe: io-media's width words, read through /proc/<pid>/as. Nothing changes.
bool run_probe(const Test &t)
{
	int pid = -1;
	std::string err = capture::find_io_media(out_path("pidin.txt").c_str(), pid);
	if (err.empty())
		err = capture::probe(pid);
	summary.push_back(std::string(t.name) + ": " + (err.empty() ? "read 720 at all three words" : err));
	if (pid > 0) {
		// How io-media runs (for a copy started by hand) and how its code is
		// mapped (can a page be patched at all); all read only.
		char cmd[64];
		std::snprintf(cmd, sizeof(cmd), "pidin -p %d arg", pid);
		proc_shell(out_path("iom_arg.txt").c_str(), 5, cmd);
		std::snprintf(cmd, sizeof(cmd), "pidin -p %d env", pid);
		proc_shell(out_path("iom_env.txt").c_str(), 5, cmd);
		proc_shell(out_path("asinfo.txt").c_str(), 5, "pidin syspage=asinfo");
		std::string merr = capture::maps(pid, out_path("iom_maps.txt").c_str());
		summary.push_back(std::string("maps: ") + (merr.empty() ? "see the maps: lines" : merr));
	}
	std::string perr = capture::phys("/mnt/efs-system/sbin/io-media-nvidia");
	summary.push_back(std::string("phys: ") + (perr.empty() ? "see the phys: line" : perr));
	return !sleep_quit(1000);
}

// iomcopy: a second io-media from RAM (iomcopy.h), next to the HMI's.
// (1) unchanged copy: starts? the original still alive? plays c3?
// (2) only if (1) played: the copy with the capture width at 800 plays c3
//     (720 wide): right edge black or frozen = the capture stops at the
//     line's end; garbled or slanted = it counts pixels (phone-mirroring 7.14)
// (3) the original plays c3 again: the HMI's io-media still works.
bool run_iomcopy(const Test &t)
{
	static unsigned graphs;
	if (access(clip_path(t.clip).c_str(), R_OK) != 0) {
		log_msg("FAIL %s: %s missing", t.name, clip_path(t.clip).c_str());
		summary.push_back(std::string(t.name) + ": clip missing");
		return !quit::stopping();
	}
	int orig = -1;
	std::string err = capture::find_io_media(out_path("pidin_iomcopy.txt").c_str(), orig);
	log_msg("%s: original io-media pid %d%s%s", t.name, orig, err.empty() ? "" : ": ",
	        err.c_str());
	std::string dir = out_path("");
	dir.pop_back();

	struct Step {
		const char *name;
		u16 width;   // 0 = the original
		const char *what;
		unsigned watch_ms;
	};
	const Step steps[] = {
		{"copy720", 720, "1 of 3: the COPY plays the 720x480 video.", 10000},
		{"copy800", 800,
		 "2 of 3: the copy capturing 800 wide plays the 720x480 video. Is the RIGHT edge "
		 "black or frozen, or is the picture garbled or slanted?",
		 20000},
		{"orig720", 0, "3 of 3: the original plays the 720x480 video again.", 8000},
	};
	bool copy_played = false;
	for (const Step &s : steps) {
		if (s.width == 800 && !copy_played) {
			log_msg("%s: skipped, the unchanged copy didn't play", s.name);
			summary.push_back(std::string(s.name) + ": skipped (copy720 didn't play)");
			continue;
		}
		ui::show(s.name, std::string(s.what) + kSeen, kFooter);
		if (sleep_quit(kLabelMs / 2))
			return false;
		ui::hide();

		iomcopy::Copy copy;
		if (s.width != 0) {
			log_msg("== %s: start the copy", s.name);
			err = copy.start(bin_dir, dir, s.width);
			if (!err.empty()) {
				log_msg("FAIL %s: %s", s.name, err.c_str());
				summary.push_back(std::string(s.name) + ": " + err);
				continue;   // copy800 then skips: copy_played stays false
			}
			// idle first: does the original (and the radio) survive the start?
			bool stop = sleep_quit(5000);
			log_msg("  after 5 s idle: copy %s, original pid %d %s", copy.alive() ? "alive" : "GONE",
			        orig, iomcopy::pid_alive(orig) ? "alive" : "GONE");
			if (stop)
				return false;
			if (!copy.alive()) {
				summary.push_back(std::string(s.name) + ": the copy exited after its start");
				continue;
			}
			iom_set_root(iomcopy::kMount);
		}
		Test step = t;
		step.name = s.name;
		step.title = s.what;
		step.kind = Kind::File;
		step.watch_ms = s.watch_ms;
		char gname[48];
		std::snprintf(gname, sizeof(gname), "hello10_%d_copy%u", (int)getpid(), ++graphs);
		unsigned te = 0;
		bool go = run_file(step, gname, &te);
		iom_set_root(nullptr);
		if (s.width == 720)
			copy_played = te > 0;
		if (s.width != 0) {
			log_msg("  after play: copy %s, original pid %d %s", copy.alive() ? "alive" : "GONE",
			        orig, iomcopy::pid_alive(orig) ? "alive" : "GONE");
			copy.stop();
		}
		if (!go)
			return false;
	}
	log_msg("RESULT %s: original io-media pid %d %s at the end", t.name, orig,
	        iomcopy::pid_alive(orig) ? "alive" : "GONE");
	return !quit::stopping();
}

bool run_test(int n, int total, const Test &t)
{
	char title[96];
	std::snprintf(title, sizeof(title), "Test %d/%d: %s", n, total, t.title);
	log_msg("== test %d/%d %s: %s (%s)", n, total, t.name, t.title, t.clip ? t.clip : "no clip");
	ui::show(title, t.what, kFooter);
	if (sleep_quit(kLabelMs))
		return false;
	if (t.kind == Kind::Copy)
		return run_iomcopy(t);
	ui::hide();
	return run_probe(t);
}

bool selected(const char *only, const Test &t)
{
	if (only == nullptr)   // the default run: every test
		return true;
	std::string list = std::string(",") + only + ",";
	return list.find(std::string(",") + t.name + ",") != std::string::npos;
}

// Leave no displayable of ours behind; the screen back to the HMI.
// The layer-manager part only: quit's hard path, while the main thread may
// still be drawing.
void release_screen()
{
	int cids[3] = {40, 16, 19};
	if (lm_open() != 0)
		return;
	lm_unregister_displayable(41);
	lm_unregister_displayable(40);
	log_msg("restore: lm_show_displayables(0, {40,16,19}): rc=%d", lm_show_displayables(0, cids, 3));
	lm_close();
}

void restore_screen()
{
	ui::hide();
	release_screen();
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
	quit::Config qc;
	qc.sd_path = sd_path;
	qc.watchdog_s = kWatchdogS;
	qc.cleanup = [](bool hard) { hard ? release_screen() : restore_screen(); };
	quit::start(qc);
	ipc_subscribe(8, on_key, nullptr, "hello10");
	log_msg("testapp-cmdline: %s", tool_path("testapp-cmdline").c_str());

	std::vector<const Test *> run;
	for (const Test &t : kTests)
		if (selected(only, t))
			run.push_back(&t);

	char intro[600];
	std::snprintf(intro, sizeof(intro),
	              "Park the car, radio on, and film the screen. %zu test(s), each with a screen "
	              "before it. Turn the main knob ONE click whenever you see the moving test "
	              "picture.\n\nIf the radio or the screen stops working afterwards, "
	              "restart the MMI (MENU + main knob + top-right key).",
	              run.size());
	ui::show("hello10: capture width", intro, kFooter);
	if (!sleep_quit(kIntroMs)) {
		int n = 0;
		for (const Test *t : run)
			if (!run_test(++n, (int)run.size(), *t))
				break;
	}
	vout::close_all();
	log_msg("== system log after the tests -> %s: %s", out_path("slog.txt").c_str(),
	        proc_status_str(proc_shell(out_path("slog.txt").c_str(), 30, "sloginfo")));

	log_msg("== summary (lag = min/median/max/last ms after the frame was pushed; knob = turns seen)");
	for (const std::string &s : summary)
		log_msg("  %s", s.c_str());
	if (!quit::stopping()) {
		ui::show_list("hello10 results (also in hello10.log)",
		              summary.empty() ? std::vector<std::string>{"no test ran"} : summary);
		sleep_quit(kPageMs);
	}
	quit::finish();
	log_msg("hello10 exit (stop: %s)", quit::name(quit::poll()));
	log_close();
	return 0;
}

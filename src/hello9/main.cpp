// hello9 - can the unit's DM642 decoder show a live H.264 stream? The
// streaming tests of docs/phone-mirroring.md (7.8), moved out of hello6.
//
// io-media (the unit's own, unpatched) plays each test through our own
// client (common/iom.c); the picture goes on screen through a video
// displayable on VCAP 0 input 1 (vout.cpp). The driver turns the knob when
// the moving test picture is visible. Tests, in order:
//
//   c5      control: a plain MP4 (720x336) from the card, seen since 21.10
//   c9      constant-sample-size MP4 (media/csmp4.py) from the card: does
//           the DSP take frames padded with a filler NAL?
//   c10g    the same layout, its header declaring 10 minutes, written to
//           the card while it plays (2 s head start)
//   live    hello9 serves the stream itself at /dev/hello9/live.mp4
//           (stream_server.cpp): header at once, frame k when its time
//           comes, reads that run ahead wait. 30 s.
//   stall   the same with two pauses (2 s at 8 s, 5 s at 16 s), the way a
//           phone's stream stutters
//
// For the live tests every read io-media makes is counted (how far ahead of
// the stream it reads, how long it waits) and every TIME event gives the
// lag: how long after a frame was released io-media reports playing it.
//
// Output: <sd>/hello9.log (every step, RESULT lines, the summary) and
// <sd>/hello9/ (io-media queries, the system log after the tests).
//
// usage: hello9 <sd_path> [test,test,...]   (default: all, in the order above)
extern "C" {
#include "log.h"
#include "proc.h"
#include "quit.h"
#include "lmgr.h"
}

#include "player.h"
#include "stream_plan.h"
#include "stream_server.h"
#include "ui.h"
#include "vout.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr unsigned kWatchdogS = 480;    // ~3.5 min run
constexpr unsigned kIntroMs = 12000;
constexpr unsigned kLabelMs = 7000;
constexpr unsigned kStartMs = 1000;     // play -> video displayable
constexpr unsigned kQueryMs = 2500;
constexpr unsigned kPageMs = 12000;
constexpr unsigned kClipS = 22;         // media/mkclips.sh D
constexpr unsigned kHeadS = 2;          // growing file: seconds written up front
constexpr const char *kLivePath = "/dev/hello9/live.mp4";
constexpr const char *kFooter = "hello9 - streaming test. Pull the SD card to stop.";

enum class Kind { File, Grow, Live };

struct Test {
	const char *name;
	const char *title;
	const char *what;
	Kind kind;
	const char *clip;
	unsigned watch_ms;
	std::vector<plan::Stall> stalls;
};

const char kSeen[] = "\n\nTurn the main knob ONE click when you see the moving test picture "
                     "(its name and a counting number).";
const char kSeenStall[] =
	"\n\nTurn the main knob ONE click when you see the moving test picture, and ONE "
	"more click each time it starts moving again after it froze.";

const std::vector<Test> kTests = {
	{"c5", "Control: plain MP4", "The 720x336 video seen on every run since 21.10.", Kind::File,
	 "c5_h264_336.mp4", 8000, {}},
	{"c9", "Constant-size MP4",
	 "Every frame padded to the same size: the layout a live stream can use.", Kind::File,
	 "c9_h264_336_cs.mp4", 8000, {}},
	{"c10g", "Constant-size MP4, growing",
	 "Its header says 10 minutes; it is written to the card while it plays.", Kind::Grow,
	 "c10_h264_336_cs_live.mp4", 10000, {}},
	{"live", "Live stream (30 s)",
	 "hello9 sends the video itself, frame by frame, like a phone would.", Kind::Live,
	 "c10_h264_336_cs_live.mp4", 30000, {}},
	{"stall", "Live stream with pauses (36 s)",
	 "The same, but the stream stops twice (2 s and 5 s), like a phone that stutters. "
	 "The picture may freeze: that is part of the test.",
	 Kind::Live, "c10_h264_336_cs_live.mp4", 36000, {{200, 2000}, {400, 5000}}},
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

// ---- growing file (c10g): the clip written to the card in real time ----

class Grow {
public:
	std::string start(const std::string &src, const std::string &dst)
	{
		FILE *f = std::fopen(src.c_str(), "rb");
		if (f == nullptr)
			return src + ": " + std::strerror(errno);
		std::fseek(f, 0, SEEK_END);
		long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		data_.resize((size_t)size);
		size_t got = std::fread(data_.data(), 1, data_.size(), f);
		std::fclose(f);
		if (got != data_.size())
			return src + ": short read";
		fd_ = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd_ < 0)
			return dst + ": " + std::strerror(errno);
		head_ = data_.size() * kHeadS / kClipS;
		ssize_t n = write(fd_, data_.data(), head_);
		done_ = n > 0 ? (size_t)n : 0;
		t0_ = log_ms();
		log_msg("  growing %s: %zu bytes, %zu written up front", dst.c_str(), data_.size(), done_);
		return "";
	}

	void tick()
	{
		if (fd_ < 0 || done_ >= data_.size())
			return;
		double frac = (log_ms() - t0_) / (kClipS * 1000.0);
		size_t want = head_ + (size_t)(frac * (double)data_.size());
		if (want > data_.size())
			want = data_.size();
		if (want <= done_)
			return;
		ssize_t n = write(fd_, data_.data() + done_, want - done_);
		if (n > 0)
			done_ += (size_t)n;
		if (done_ >= data_.size())
			log_msg("  growing file complete after %u ms", log_ms() - t0_);
	}

	~Grow()
	{
		if (fd_ >= 0)
			close(fd_);
	}

private:
	std::vector<uint8_t> data_;
	int fd_ = -1;
	size_t head_ = 0, done_ = 0;
	unsigned t0_ = 0;
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

// Run one test; false if hello9 should stop.
bool run_test(int n, int total, const Test &t)
{
	static unsigned graphs;
	char title[96];
	std::snprintf(title, sizeof(title), "Test %d/%d: %s", n, total, t.title);
	log_msg("== test %d/%d %s: %s (%s)", n, total, t.name, t.title, t.clip);

	const std::string src = clip_path(t.clip);
	if (access(src.c_str(), R_OK) != 0) {
		log_msg("FAIL %s: %s missing", t.name, src.c_str());
		summary.push_back(std::string(t.name) + ": clip missing");
		return true;
	}

	ui::show(title, std::string(t.what) + (t.stalls.empty() ? kSeen : kSeenStall), kFooter);
	if (sleep_quit(kLabelMs))
		return false;
	ui::hide();

	// the source
	std::string url = src, err;
	Grow grow;
	StreamServer server;
	plan::CsMp4 m;
	plan::Schedule schedule(25.0, t.stalls);
	if (t.kind == Kind::Grow) {
		url = out_path("grow_c10.mp4");
		err = grow.start(src, url);
	} else if (t.kind == Kind::Live) {
		std::vector<uint8_t> head(65536);
		FILE *f = std::fopen(src.c_str(), "rb");
		size_t got = f ? std::fread(head.data(), 1, head.size(), f) : 0;
		if (f)
			std::fclose(f);
		struct stat sb = {};
		stat(src.c_str(), &sb);
		err = plan::parse(head.data(), got, (uint64_t)sb.st_size, m);
		if (err.empty()) {
			schedule = plan::Schedule(m.fps(), t.stalls);
			log_msg("  stream: %u-byte frames, %u declared (%.0f s), %u in the file, %.2f fps, "
			        "%llu bytes",
			        m.sample_size, m.declared, m.declared / m.fps(), m.present, m.fps(),
			        (unsigned long long)m.total());
			for (const plan::Stall &s : t.stalls)
				log_msg("  stall: %u ms before frame %u (%.1f s into the stream)", s.ms,
				        s.at_frame, s.at_frame / m.fps());
			url = kLivePath;
			err = server.start(kLivePath, src.c_str(), m, schedule);
		}
	}
	if (!err.empty()) {
		log_msg("FAIL %s: source: %s", t.name, err.c_str());
		summary.push_back(std::string(t.name) + ": " + err);
		return true;
	}

	// the player
	char gname[48];
	std::snprintf(gname, sizeof(gname), "hello9_%d_%u", (int)getpid(), ++graphs);
	Player player;
	if (t.kind == Kind::Live)
		server.go();                       // before attach: io-media reads at once
	err = player.open(gname, url);
	if (err.empty())
		err = player.play();
	const unsigned t_play = log_ms();
	if (!err.empty()) {
		log_msg("FAIL %s: %s", t.name, err.c_str());
		summary.push_back(std::string(t.name) + ": " + err);
		player.close();
		server.stop();
		return quit_poll() == QUIT_NONE;
	}

	Lags lags;
	auto on_time = [&](unsigned t_since_play, int64_t pos) {
		if (t.kind == Kind::Live) {
			uint64_t frame = (uint64_t)((double)pos * m.fps() / 1000.0);
			lags.add((int64_t)server.elapsed_ms() - (int64_t)schedule.release_ms(frame));
		} else {
			lags.add((int64_t)t_since_play - pos);
		}
	};

	// watch: video on after kStartMs, events, the growing file, server stats
	Query q;
	std::thread query;
	bool video_on = false, stop = false;
	unsigned t_video = 0, last_stats = 0;
	while (!stop) {
		unsigned now = log_ms() - t_play;
		if (now >= kStartMs + t.watch_ms)
			break;
		if (!video_on && now >= kStartMs) {
			query = std::thread([&q, gname, t] {
				q = query_graph(gname, std::string("query_") + t.name + ".log");
			});
			vout::show();
			video_on = true;
			t_video = log_ms();
		}
		grow.tick();
		player.poll(on_time);
		if (t.kind == Kind::Live) {
			log_notes(server);
			if (now - last_stats >= 1000) {
				last_stats = now;
				StreamStats s = server.stats();
				long long served_frame =
					s.served_end > m.data_off
						? (long long)((s.served_end - m.data_off - 1) / m.sample_size)
						: -1;
				log_msg("  t=%5.1f s: released %llu frames, served to frame %lld, %u reads "
				        "(%u waited, max %u ms), lead max %lld last %lld ms, %u waiting, "
				        "pos %lld ms, lag %lld ms",
				        server.elapsed_ms() / 1000.0, (unsigned long long)server.released(),
				        served_frame, s.reads, s.waited, s.wait_ms_max,
				        (long long)(s.lead_ms_max == INT64_MIN ? 0 : s.lead_ms_max),
				        (long long)s.lead_ms_last, s.waiting_now,
				        (long long)player.stats().max_pos_ms, (long long)lags.last);
			}
		}
		stop = sleep_quit(20);
	}

	// teardown: picture off, stream ended, graph gone, server gone
	if (video_on)
		vout::hide();
	server.finish();
	player.poll(on_time);
	player.close();
	if (query.joinable())
		query.join();
	log_notes(server);
	StreamStats s = server.stats();
	server.stop();

	// results
	const PlayStats &ps = player.stats();
	std::vector<unsigned> knobs = video_on ? knobs_since(t_video) : std::vector<unsigned>();
	std::string knob_str;
	for (unsigned k : knobs) {
		char b[24];
		std::snprintf(b, sizeof(b), "%s%.1f", knob_str.empty() ? "" : ",", (k + (t_video - t_play)) / 1000.0);
		knob_str += b;
	}
	log_msg("RESULT %s: playing_ms=%u first_time_ms=%u time_events=%u pos_ms=%lld errors=%u%s%s "
	        "eof=%d video=%d %ux%u knob=%zu at_s=[%s] lag=%s",
	        t.name, ps.playing_ms, ps.first_time_ms, ps.time_events, (long long)ps.max_pos_ms,
	        ps.error_events, ps.error_events ? " first: " : "", ps.first_error.c_str(),
	        ps.input_eof, q.video, q.w, q.h, knobs.size(), knob_str.c_str(), lags.str().c_str());
	if (t.kind == Kind::Live)
		log_msg("RESULT %s stream: opens=%u reads=%u preads=%u bytes=%llu read=%u..%u "
		        "waited=%u wait_max=%u ms wait_avg=%llu ms lead_max=%lld ms unblocked=%u far=%u "
		        "eof=%u",
		        t.name, s.opens, s.reads, s.preads, (unsigned long long)s.bytes, s.min_read,
		        s.max_read, s.waited, s.wait_ms_max,
		        (unsigned long long)(s.waited ? s.wait_ms_sum / s.waited : 0),
		        (long long)(s.lead_ms_max == INT64_MIN ? 0 : s.lead_ms_max), s.unblocked, s.far,
		        s.eof);

	char line[160];
	std::snprintf(line, sizeof(line), "%-6s TIME %3u pos %5.1fs knob %zu%s%s", t.name,
	              ps.time_events, ps.max_pos_ms < 0 ? 0.0 : ps.max_pos_ms / 1000.0, knobs.size(),
	              t.kind == Kind::Live ? "  lag med " : "",
	              t.kind == Kind::Live ? lags.str().c_str() : "");
	summary.push_back(line);
	if (t.kind == Kind::Live) {
		std::snprintf(line, sizeof(line), "       reads %u, waited %u (max %u ms), read-ahead %lld ms",
		              s.reads, s.waited, s.wait_ms_max,
		              (long long)(s.lead_ms_max == INT64_MIN ? 0 : s.lead_ms_max));
		summary.push_back(line);
	}
	return quit_poll() == QUIT_NONE;
}

bool selected(const char *only, const char *name)
{
	if (only == nullptr)
		return true;
	std::string list = std::string(",") + only + ",";
	return list.find(std::string(",") + name + ",") != std::string::npos;
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
	for (const Test &t : kTests)
		if (selected(only, t.name))
			run.push_back(&t);

	char intro[600];
	std::snprintf(intro, sizeof(intro),
	              "Park the car. %zu test videos, one after the other, with a screen before "
	              "each. Turn the main knob ONE click whenever you see the moving test picture. "
	              "The last two run for about 30 s; in the last one the picture may freeze "
	              "twice, which is part of the test.\n\nLeave the card in until the results "
	              "screen has gone.",
	              run.size());
	ui::show("hello9: streaming test (~3.5 min)", intro, kFooter);
	if (!sleep_quit(kIntroMs)) {
		int n = 0;
		for (const Test *t : run)
			if (!run_test(++n, (int)run.size(), *t))
				break;
	}
	vout::close_all();
	log_msg("== system log after the tests -> %s: %s", out_path("slog.txt").c_str(),
	        proc_status_str(proc_shell(out_path("slog.txt").c_str(), 30, "sloginfo")));

	log_msg("== summary (lag = min/median/max/last ms after release; knob = turns seen)");
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

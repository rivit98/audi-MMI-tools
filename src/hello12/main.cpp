// hello12 - live SBC audio through io-media's Bluetooth reader on the DM642
// (docs/bt-audio-path.md). io-media turns the input "sbc:<path>" into
// "BT@44100.16.2.SBC:<path>" and decodes it on the DSP; the path can be
// our own resource manager, so an app can play a live stream without the
// Bluetooth stack. The output is the one audible mixer output,
// TDM1_20_SAD, so the radio goes quiet while it plays (expected).
//
// Steps, in order (default "wfsS"):
//   w  read only: list /dev/wms, open the Bluetooth stack's player1 and
//      ask it what io-media asks (connected, codec, rate); in a child
//      process with a deadline (wmsprobe.h)
//   f  control: the ping clip (a1) from the card through "sbc:<file>"
//   s  live: the sweep clip (a2) served by hello12's resource manager in
//      real time, reads answered with what is out (short reads)
//   S  the same, each read held until all it asked for is out
//   W  stand-in for the Bluetooth stack: hello12's resource manager takes
//      /dev/wms/player1 (newest attach wins) and serves the sweep clip in a
//      loop with the stack's devctl answers (wms.h). With a phone playing
//      music over Bluetooth, the driver switches Media to another source
//      and back: the MME's a2dp_player graph opens the path again and
//      should play the rising tone instead of the music. After 2 minutes
//      the path is detached (the stack's own node is back), and the driver
//      switches once more: the phone's music should return.
//
// The listener says what was heard for each step (on the phone video:
// the screen names the step and the sound). Every io-media event, every
// request io-media makes of the stream (opens, read sizes, waits,
// devctl codes) and the system log go to var/hello12.log and
// var/hello12/.
//
// usage: hello12 <sd_path> [steps]       (default wfsSW)
//        hello12 <sd_path> @wms      (the step w child; @wmsW: step W's)
#include "types.h"
extern "C" {
#include "iom.h"
#include "lmgr.h"
#include "log.h"
#include "proc.h"
#include "quit.h"
}

#include "sbc.h"
#include "sbcsrv.h"
#include "ui.h"
#include "wms.h"
#include "wmsprobe.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr unsigned kWatchdogS = 600;
constexpr unsigned kIntroMs = 10000;
constexpr unsigned kLabelMs = 5000;
constexpr unsigned kTailMs = 8000;       // after the clip's length: wait for FINISHED
constexpr unsigned kWmsDeadlineMs = 6000;
constexpr unsigned kPrebufferFrames = 69; // ~200 ms of 44.1 kHz SBC
constexpr const char *kOutput = "mixer:DSPOUT_TDM1_20_SAD?delay=130";
constexpr const char *kLivePath = "/dev/mmi_apps/live.sbc";
constexpr const char *kWmsPath = "/dev/wms/player1";
constexpr unsigned kStandInMs = 120000;  // the stand-in serves this long
constexpr unsigned kStandInLeadMs = 20000; // its instructions, before our screen goes
constexpr unsigned kBackMs = 40000;      // after the detach: the driver switches back
constexpr const char *kPingClip = "a1_sbc_ping.sbc";
constexpr const char *kSweepClip = "a2_sbc_sweep.sbc";
constexpr const char *kFooter = "hello12 - Bluetooth audio path test. Pull the SD card to stop.";
constexpr const char *kDefaultSteps = "wfsSW";

const char *sd_path = "";
std::vector<std::string> summary;

std::string out_path(const char *name)
{
	return std::string(var_dir(sd_path)) + "/hello12/" + name;
}

std::string clip_path(const char *clip)
{
	return std::string(sd_path) + "/clips/" + clip;
}

bool sleep_quit(unsigned ms)
{
	unsigned t0 = log_ms();
	while (quit_poll() == QUIT_NONE) {
		if (log_ms() - t0 >= ms)
			return false;
		usleep(20000);
	}
	return true;
}

bool load_file(const std::string &path, std::vector<u8> &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	out.clear();
	u8 buf[16384];
	usize n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.insert(out.end(), buf, buf + n);
	std::fclose(f);
	return true;
}

// One clip, indexed.
struct Clip {
	std::vector<u8> data;
	std::vector<u32> offs;
	sbc::Header h;
	unsigned ms = 0;
};

std::string load_clip(const char *name, Clip &c)
{
	std::string path = clip_path(name);
	if (!load_file(path, c.data))
		return path + ": " + std::strerror(errno);
	std::string err = sbc::index(c.data.data(), c.data.size(), c.offs, &c.h);
	if (!err.empty())
		return path + ": " + err;
	c.ms = (unsigned)((u64)c.offs.size() * c.h.samples * 1000u / c.h.rate);
	log_msg("clip %s: %zu bytes, %zu frames of %zu bytes, %u Hz, %u ch, mode %u, %u blocks, "
	        "%u subbands, bitpool %u, %u ms",
	        name, c.data.size(), c.offs.size(), c.h.frame_len, c.h.rate, c.h.channels, c.h.mode,
	        c.h.blocks, c.h.subbands, c.h.bitpool, c.ms);
	return "";
}

const char *status_name(u32 s)
{
	switch (s) {
	case 0: return "stopped";
	case 1: return "1";
	case 2: return "2";
	case 3: return "paused";
	case 4: return "playing";
	default: return "?";
	}
}

// Log io-media's events; returns true once FINISHED or ERROR came.
// *first_time_ms: log_ms() of the first TIME event (0 = none yet).
bool drain_events(struct iom *g, unsigned t_play, unsigned *first_time_ms, unsigned *times,
                  i64 *last_pos, bool *error)
{
	u8 buf[1024];
	int n = iom_read_events(g, buf, sizeof(buf));
	bool done = false;
	for (int at = 0; n > 0 && at + 8 <= n;) {
		u32 size, type;
		std::memcpy(&size, buf + at, 4);
		std::memcpy(&type, buf + at + 4, 4);
		if (size < 8 || at + (int)size > n)
			break;
		const u8 *pl = buf + at + 8;
		unsigned dt = log_ms() - t_play;
		switch (type) {
		case IOM_EV_TIME:
			if (size >= 32) {
				i64 cur;
				std::memcpy(&cur, pl + 8, 8);
				*last_pos = cur;
				if (*first_time_ms == 0) {
					*first_time_ms = log_ms();
					log_msg("  +%u ms: TIME %lld ms (first)", dt, (long long)cur);
				}
				(*times)++;
			}
			break;
		case IOM_EV_STATUS:
			if (size >= 20) {
				u32 st;
				std::memcpy(&st, pl + 8, 4);
				log_msg("  +%u ms: STATUS %u (%s)", dt, st, status_name(st));
			}
			break;
		case IOM_EV_INPUT_EOF:
			log_msg("  +%u ms: INPUT_EOF", dt);
			break;
		case IOM_EV_FINISHED:
			log_msg("  +%u ms: FINISHED", dt);
			done = true;
			break;
		case IOM_EV_ERROR: {
			struct iom_err e{};
			iom_error_info(g, &e);
			log_msg("  +%u ms: ERROR, iom_errno 0x%x, type %u, value %llu", dt, e.iom_errno,
			        e.value_type, (unsigned long long)e.value);
			*error = true;
			done = true;
			break;
		}
		default:
			log_msg("  +%u ms: event %u (%u bytes)", dt, type, size);
			break;
		}
		at += (int)size;
	}
	return done;
}

void log_notes(SbcServer &srv)
{
	for (const std::string &s : srv.take_notes())
		log_msg("  srv: %s", s.c_str());
}

// Play url on a trackplayer graph for up to clip_ms + kTailMs. srv (may be
// null) is the live stream behind it. Returns false if a stop was asked.
bool play(const char *step, const char *url, unsigned clip_ms, SbcServer *srv, std::string *result)
{
	struct iom g;
	char name[32];
	std::snprintf(name, sizeof(name), "hello12_%s_%d", step, (int)getpid());
	int rc = iom_create(&g, name);
	if (rc != 0) {
		*result = std::string("graph: ") + std::strerror(rc);
		log_msg("FAIL %s: iom_create %s: %s", step, name, std::strerror(rc));
		return true;
	}
	bool stop = false;
	iom_set_time_notify(&g, 500);
	rc = iom_attach_output(&g, kOutput);
	log_msg("%s: attach output %s -> %s (id %d)", step, kOutput, rc ? std::strerror(rc) : "ok",
	        g.output);
	if (rc == 0) {
		unsigned t = log_ms();
		rc = iom_attach_input(&g, 1, url);
		log_msg("%s: attach input %s -> %s after %u ms", step, url, rc ? std::strerror(rc) : "ok",
		        log_ms() - t);
		if (rc != 0) {
			struct iom_err e{};
			iom_error_info(&g, &e);
			log_msg("%s: error info: iom_errno 0x%x, type %u, value %llu", step, e.iom_errno,
			        e.value_type, (unsigned long long)e.value);
		}
	}
	if (srv)
		log_notes(*srv);
	if (rc == 0) {
		unsigned t_play = log_ms();
		rc = iom_play(&g);
		log_msg("%s: play -> %s", step, rc ? std::strerror(rc) : "ok");
		unsigned first_time = 0, times = 0;
		i64 pos = -1;
		bool error = false, done = false;
		unsigned last_stats = t_play;
		while (rc == 0 && !done) {
			if (quit_poll() != QUIT_NONE) {
				stop = true;
				break;
			}
			done = drain_events(&g, t_play, &first_time, &times, &pos, &error);
			if (log_ms() - t_play > clip_ms + kTailMs) {
				log_msg("%s: no FINISHED %u ms after play: stopping", step, log_ms() - t_play);
				break;
			}
			if (srv && log_ms() - last_stats >= 5000) {
				last_stats = log_ms();
				SbcStats s = srv->stats();
				log_notes(*srv);
				log_msg("  srv: +%u ms: %u reads (%u..%u bytes), %llu bytes sent, %u/%u frames out, "
				        "%u held (max %u ms), %u short, %u devctls",
				        last_stats - t_play, s.reads, s.min_read, s.max_read,
				        (unsigned long long)s.bytes, s.frames_out, s.frames, s.waited,
				        s.wait_ms_max, s.short_reads, s.devctls);
			}
			usleep(50000);
		}
		unsigned played = log_ms() - t_play;
		char line[200];
		std::snprintf(line, sizeof(line), "%s: %s, %u TIME events, last %lld ms, first after %d ms, "
		              "ran %u ms of a %u ms clip",
		              step, error ? "io-media ERROR" : done ? "FINISHED" : "no FINISHED", times,
		              (long long)pos, first_time ? (int)(first_time - t_play) : -1, played, clip_ms);
		*result = line;
		log_msg("RESULT %s", line);
	} else if (result->empty()) {
		*result = std::string(step) + ": not played (" + std::strerror(rc) + ")";
		log_msg("RESULT %s", result->c_str());
	}
	iom_close(&g);
	if (srv)
		log_notes(*srv);
	return !stop;
}

// Step w: the read-only /dev/wms probe in a child with a deadline. Its log
// is hello12/wms.log, or hello12/wms_W.log for step W's look before the
// stand-in (W true).
void step_wms(const char *self, bool W = false)
{
	std::string log = out_path(W ? "wms_W_out.log" : "wms_out.log");
	char *argv[] = {const_cast<char *>(self), const_cast<char *>(sd_path),
	                const_cast<char *>(W ? "@wmsW" : "@wms"), nullptr};
	pid_t pid = proc_spawn(log.c_str(), argv, nullptr);
	if (pid < 0) {
		log_msg("FAIL w: spawn: %s", std::strerror(errno));
		summary.push_back("w: child not started");
		return;
	}
	log_msg("w: child pid %d, deadline %u ms, its log: %s", (int)pid, kWmsDeadlineMs,
	        out_path(W ? "wms_W.log" : "wms.log").c_str());
	int status = 0;
	if (proc_wait(pid, kWmsDeadlineMs, &status) == 0) {
		log_msg("RESULT w: child %s", proc_status_str(status));
		summary.push_back(std::string(W ? "W: probe " : "w: probe ") + proc_status_str(status) +
		                  (W ? " (hello12/wms_W.log)" : " (hello12/wms.log)"));
	} else {
		log_msg("RESULT w: child still running after %u ms (a server did not reply): killed",
		        kWmsDeadlineMs);
		kill(pid, SIGKILL);
		summary.push_back(W ? "W: probe HUNG and was killed" : "w: probe HUNG and was killed");
	}
}

// Give the screen to the HMI (our cid 40 away, the HMI's displayables shown).
void restore_screen();

// Step W: the stand-in for /dev/wms/player1. Returns false if a stop was asked.
bool step_standin(const char *self, const Clip &clip)
{
	log_msg("W: the stack's own node first (read only):");
	step_wms(self, true);
	SbcServer srv;
	SbcWms w{wms::kCodecSbc, wms::kRate44100};
	std::string err = srv.start(kWmsPath, &clip.data, &clip.offs, clip.h.samples, clip.h.rate,
	                            kPrebufferFrames, true, &w);
	if (!err.empty()) {
		log_msg("FAIL W: stand-in: %s", err.c_str());
		summary.push_back("W: stand-in not attached: " + err);
		return true;
	}
	log_msg("W: stand-in attached at %s (SBC, 44100 Hz, the sweep looped, short reads)", kWmsPath);
	ui::show("Bluetooth stand-in: switch now",
	         "hello12 now answers for the phone's Bluetooth audio.\n\nPress MEDIA, choose another "
	         "source (radio), then Bluetooth audio again. Instead of the phone's music you should "
	         "hear the RISING TONE (again every 2 s).\n\nThis screen goes away in 20 s; the "
	         "stand-in stays for 2 minutes.",
	         kFooter);
	bool stop = sleep_quit(kStandInLeadMs);
	if (!stop) {
		restore_screen();
		log_msg("W: screen given to the HMI, serving for %u ms", kStandInMs);
	}
	unsigned t0 = log_ms(), last = t0;
	while (!stop && log_ms() - t0 < kStandInMs) {
		stop = sleep_quit(500);
		log_notes(srv);
		if (log_ms() - last >= 10000) {
			last = log_ms();
			SbcStats s = srv.stats();
			log_msg("  srv: +%u ms: %u opens, %u closes, %u devctls (%u answered), %u reads "
			        "(%u..%u bytes), %llu bytes sent, %u frames out, %u held (max %u ms), %u "
			        "short, %u unblocks",
			        last - t0, s.opens, s.closes, s.devctls, s.devctls_answered, s.reads, s.min_read,
			        s.max_read, (unsigned long long)s.bytes, s.frames_out, s.waited, s.wait_ms_max,
			        s.short_reads, s.unblocks);
		}
	}
	SbcStats s = srv.stats();
	srv.stop();
	log_notes(srv);
	log_msg("W: stand-in detached after %u ms: %s is the stack's again", log_ms() - t0, kWmsPath);
	char line[260];
	std::snprintf(line, sizeof(line),
	              "W stand-in: %u opens, %u closes, %u devctls (%u answered), %u reads (%u..%u "
	              "bytes), %llu bytes = %.1f s of SBC sent, %u held (max %u ms), %u short, %u "
	              "unblocks, first read at %d ms (log clock)",
	              s.opens, s.closes, s.devctls, s.devctls_answered, s.reads, s.min_read, s.max_read,
	              (unsigned long long)s.bytes,
	              (f64)s.bytes / (f64)clip.h.frame_len * clip.h.samples / clip.h.rate, s.waited,
	              s.wait_ms_max, s.short_reads, s.unblocks,
	              s.first_read_ms ? (int)s.first_read_ms : -1);
	log_msg("RESULT %s", line);
	summary.push_back(line);
	if (stop)
		return false;
	ui::show("Bluetooth stand-in removed",
	         "The phone's Bluetooth audio is the stack's again.\n\nPress MEDIA, choose another "
	         "source, then Bluetooth audio again: does the phone's MUSIC come back?\n\nThis "
	         "screen goes away in 10 s and comes back with the results.",
	         kFooter);
	if (sleep_quit(10000))
		return false;
	restore_screen();
	log_msg("W: waiting %u ms for the switch back", kBackMs);
	return !sleep_quit(kBackMs);
}

bool label(int n, int count, const char *title, const char *what)
{
	char t[120];
	std::snprintf(t, sizeof(t), "Step %d of %d: %s", n, count, title);
	ui::show(t, what, kFooter);
	log_msg("== step %d/%d: %s", n, count, title);
	return !sleep_quit(kLabelMs);
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

} // namespace

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path> [steps|@wms|@wmsW]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	const char *steps = argc > 2 && argv[2][0] ? argv[2] : kDefaultSteps;

	if (std::strcmp(steps, "@wms") == 0 || std::strcmp(steps, "@wmsW") == 0) {
		// the step w / W child: its own log (stdout goes to wms[_W]_out.log)
		log_open(out_path(steps[4] == 'W' ? "wms_W.log" : "wms.log").c_str());
		int rc = wms_probe_main();
		log_close();
		return rc;
	}

	mkdir(out_path("").c_str(), 0777);
	std::string logpath = std::string(var_dir(sd_path)) + "/hello12.log";
	if (log_open(logpath.c_str()) != 0)
		std::fprintf(stderr, "hello12: cannot open %s, logging to stdout only\n", logpath.c_str());
	log_msg("hello12 start, pid=%d, steps %s", (int)getpid(), steps);
	quit_init(sd_path, kWatchdogS, nullptr, nullptr);

	int count = (int)std::strlen(steps), n = 0;
	char intro[600];
	std::snprintf(intro, sizeof(intro),
	              "Radio ON at a normal volume, and film with sound. %d steps, each screen "
	              "names the sound to listen for. The last one needs the phone playing music "
	              "over Bluetooth. The radio goes quiet while a "
	              "sound plays; that is expected.\n\nSay out loud what you hear: clean, "
	              "choppy, too fast or slow, or nothing.\n\nLeave the card in until the results "
	              "screen has gone.",
	              count);
	ui::show("hello12: Bluetooth audio path (~6 min)", intro, kFooter);
	bool go = !sleep_quit(kIntroMs);

	Clip ping, sweep;
	std::string ping_err, sweep_err;
	if (std::strchr(steps, 'f'))
		ping_err = load_clip(kPingClip, ping);
	if (std::strchr(steps, 's') || std::strchr(steps, 'S') || std::strchr(steps, 'W'))
		sweep_err = load_clip(kSweepClip, sweep);

	for (const char *p = steps; go && *p; p++) {
		std::string result;
		char step[2] = {*p, 0};
		n++;
		switch (*p) {
		case 'w':
			if (!label(n, count, "Bluetooth stack's devices (no sound)",
			           "hello12 lists /dev/wms and asks the Bluetooth stack's player1 device "
			           "whether a phone is streaming. Nothing to listen for."))
				go = false;
			else
				step_wms(argv[0]);
			break;
		case 'f': {
			if (!label(n, count, "PINGS from a file (control)",
			           "A short ping every 2 seconds for 20 s, decoded by the MMI's DSP from an "
			           "SBC file on the card.\n\nDo you hear the pings?")) {
				go = false;
				break;
			}
			if (!ping_err.empty()) {
				log_msg("FAIL f: %s", ping_err.c_str());
				summary.push_back("f: clip missing: " + ping_err);
				break;
			}
			std::string url = "sbc:" + clip_path(kPingClip);
			ui::show("Listening: PINGS (file)", "A ping every 2 seconds, 20 s.", kFooter);
			go = play(step, url.c_str(), ping.ms, nullptr, &result);
			summary.push_back(result);
			break;
		}
		case 's':
		case 'S': {
			bool partial = *p == 's';
			if (!label(n, count,
			           partial ? "RISING TONE, live (short reads)" : "RISING TONE, live (full reads)",
			           "A tone that rises for 2 seconds, then starts again low, for 20 s. "
			           "hello12 sends it in real time, as a phone would.\n\nIs it smooth, "
			           "choppy, or silent?")) {
				go = false;
				break;
			}
			if (!sweep_err.empty()) {
				log_msg("FAIL %c: %s", *p, sweep_err.c_str());
				summary.push_back(std::string(step) + ": clip missing: " + sweep_err);
				break;
			}
			SbcServer srv;
			std::string err = srv.start(kLivePath, &sweep.data, &sweep.offs, sweep.h.samples,
			                            sweep.h.rate, kPrebufferFrames, partial);
			if (!err.empty()) {
				log_msg("FAIL %c: server: %s", *p, err.c_str());
				summary.push_back(std::string(step) + ": server: " + err);
				break;
			}
			log_msg("%c: serving %s (%s reads, prebuffer %u frames)", *p, kLivePath,
			        partial ? "short" : "full", kPrebufferFrames);
			std::string url = std::string("sbc:") + kLivePath;
			ui::show("Listening: RISING TONE (live)", "A rising tone, again every 2 s, 20 s.",
			         kFooter);
			go = play(step, url.c_str(), sweep.ms, &srv, &result);
			SbcStats s = srv.stats();
			char line[240];
			std::snprintf(line, sizeof(line),
			              "%c stream: %u opens, %u reads (%u..%u bytes, %u preads), %llu of %zu "
			              "bytes sent, %u held (max %u ms), %u short, %u EOF, %u devctls, %u "
			              "unblocks, first read at %d ms (log clock)",
			              *p, s.opens, s.reads, s.min_read, s.max_read, s.preads,
			              (unsigned long long)s.bytes, sweep.data.size(), s.waited, s.wait_ms_max,
			              s.short_reads, s.eof, s.devctls, s.unblocks,
			              s.first_read_ms ? (int)s.first_read_ms : -1);
			log_msg("RESULT %s", line);
			srv.stop();
			summary.push_back(result);
			summary.push_back(line);
			break;
		}
		case 'W': {
			if (!label(n, count, "BLUETOOTH STAND-IN (phone music needed)",
			           "Needs the phone connected over Bluetooth, playing music in Media -> "
			           "Bluetooth audio. hello12 takes over the stream the phone sends, for 2 "
			           "minutes: the next screen says when to switch.")) {
				go = false;
				break;
			}
			if (!sweep_err.empty()) {
				log_msg("FAIL W: %s", sweep_err.c_str());
				summary.push_back("W: clip missing: " + sweep_err);
				break;
			}
			go = step_standin(argv[0], sweep);
			break;
		}
		default:
			log_msg("unknown step '%c'", *p);
			break;
		}
	}

	log_msg("== system log -> %s: %s", out_path("slog.txt").c_str(),
	        proc_status_str(proc_shell(out_path("slog.txt").c_str(), 30, "sloginfo")));
	log_msg("== summary");
	for (const std::string &s : summary)
		log_msg("  %s", s.c_str());
	if (quit_poll() == QUIT_NONE) {
		ui::show_list("hello12 results (also in hello12.log)",
		              summary.empty() ? std::vector<std::string>{"no step ran"} : summary);
		sleep_quit(12000);
	}
	quit_request(QUIT_TIMER);
	restore_screen();
	log_msg("hello12 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

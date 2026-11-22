// runner - a menu of the test apps on the card: turn the main knob to choose
// one, press it to run it. The app runs as it would under run.sh (same
// arguments, timeout, output file, verdict, summary line, slog and core
// files); then the menu comes back with its verdict. "Leave the menu" ends
// the runner, and with it run.sh's run.
//
// The menu lists every app in apps.conf whose binary is in the runner's own
// directory (run.sh copies the card's bin/ to /tmp/mmi_apps), in apps.conf's
// order. Input: src/buttons (Hub fed by quit.c's ch8 thread, EventQueue).
//
// Apps are started with QNX's spawn(), not fork + exec, system() or ksh
// (README.md: why). The runner closes its screen first and opens it again
// after the app. While the HMI shows the rear camera the menu stays away.
//
// Output: <sd>/var/runner.log (every start, key and result), and per app
// what run.sh writes: var/<app>_out.log (+ "=== " lines), var/summary.txt,
// var/<app>_slog.txt, core files.
//
// usage: runner <sd_path>
#include "types.h"

extern "C" {
#include "log.h"
#include "quit.h"
#include "screen.h"
#include "text.h"
#include "font_big.h"
#include "font_small.h"
}

#include "camera.h"
#include "conf.h"
#include "hub.h"
#include "menu.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr unsigned kRedrawMs = 100;
constexpr unsigned kReshowMs = 1000;
constexpr unsigned kWaitMs = 200;
constexpr unsigned kKillGraceMs = 15000;   // run.sh's KILL_GRACE after a timeout
constexpr unsigned kQuitGraceMs = 7000;    // quit.c ends us QUIT_HARD_S after a stop
constexpr unsigned kPostS = 60;            // the slog + core collection
constexpr int kMargin = 20;
constexpr int kRowH = 40;
constexpr int kListY = 56;
constexpr int kRows = 7;
constexpr int kTextY = kListY + kRows * kRowH + 8;
constexpr const char *kSelf = "runner";
constexpr const char *kDefaultBin = "/tmp/mmi_apps";

constexpr u32 C_BG = 0xff101820u;
constexpr u32 C_TEXT = 0xffe8e8e8u;
constexpr u32 C_DIM = 0xff8a929eu;
constexpr u32 C_TITLE = 0xffffd040u;
constexpr u32 C_SEL = 0xff24405cu;
constexpr u32 C_OK = 0xff4cd964u;
constexpr u32 C_WARN = 0xffffb020u;
constexpr u32 C_BAD = 0xffff5a50u;
constexpr u32 C_BAR = 0xff1c2836u;

struct Item {
	runner::AppConf app;    // name empty: "Leave the menu"
	std::string verdict;    // the last run's, "" = not run
	std::string detail;     // "exit 0, 183 s"
};

std::string sd, var, bin;
std::vector<Item> items;
std::string status = "Pull the SD card to stop. The MMI also reacts to the knob.";
bool ui = false, cam_was = false, dirty = true;

std::string read_file(const std::string &path)
{
	std::string s;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return s;
	char buf[4096];
	usize n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	std::fclose(f);
	return s;
}

void append_file(const std::string &path, const std::string &text)
{
	FILE *f = std::fopen(path.c_str(), "a");
	if (!f)
		return;
	std::fputs(text.c_str(), f);
	std::fclose(f);
}

bool executable(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// ---- starting an app ----

// spawn() argv[0] with stdin /dev/null and stdout + stderr into out_path
// (truncated or appended). Only those three fds reach the child (QNX:
// fd_count > 0 inherits nothing else, so not our ch8, log or screen fds), it
// gets an empty signal mask (ours blocks SIGTERM for quit.c's sigwait) and a
// process group of its own (the KILL after a timeout takes its tools too).
pid_t start(const std::vector<std::string> &args, const std::string &out_path, bool truncate)
{
	int out = open(out_path.c_str(), O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND), 0644);
	if (out < 0)
		return -1;
	int in = open("/dev/null", O_RDONLY);
	int fd_map[3] = {in >= 0 ? in : SPAWN_FDCLOSED, out, out};

	struct inheritance inh;
	std::memset(&inh, 0, sizeof(inh));
	inh.flags = SPAWN_SETGROUP | SPAWN_SETSIGMASK;
	inh.pgroup = SPAWN_NEWPGROUP;
	sigemptyset(&inh.sigmask);

	std::vector<char *> argv;
	for (const std::string &a : args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	pid_t pid = spawn(argv[0], 3, fd_map, &inh, argv.data(), environ);
	int err = errno;
	close(out);
	if (in >= 0)
		close(in);
	errno = err;
	return pid;
}

void nap(unsigned ms)
{
	usleep(ms * 1000);
}

// Wait for pid; TERM at timeout_s or when we are asked to stop, KILL (the
// whole group) after the grace time. Returns the wait status.
int await(pid_t pid, unsigned timeout_s, bool &timed_out, bool &stopped)
{
	unsigned t0 = log_ms(), term_at = 0;
	bool termed = false, killed = false;
	int st = 0;

	timed_out = stopped = false;
	for (;;) {
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid)
			return st;
		if (r < 0 && errno != EINTR) {
			log_msg("waitpid %d: %s", (int)pid, std::strerror(errno));
			return 0;
		}
		unsigned now = log_ms();
		if (!termed && quit_poll() != QUIT_NONE) {
			stopped = true;
			log_msg("stop requested (%s): TERM %d", quit_name(quit_poll()), (int)pid);
		} else if (!termed && now - t0 >= timeout_s * 1000) {
			timed_out = true;
			log_msg("timeout %u s: TERM %d", timeout_s, (int)pid);
		}
		if (!termed && (stopped || timed_out)) {
			kill(pid, SIGTERM);
			termed = true;
			term_at = now;
		}
		if (termed && !killed && now - term_at >= (stopped ? kQuitGraceMs : kKillGraceMs)) {
			log_msg("still running: KILL group %d", (int)pid);
			kill(-pid, SIGKILL);
			kill(pid, SIGKILL);
			killed = true;
		}
		nap(kWaitMs);
	}
}

// run.sh's crash evidence: the system log, and core files moved to the card.
void collect(const std::string &app, const std::string &out_path)
{
	std::string cmd = "sloginfo > '" + var + "/" + app + "_slog.txt' 2>&1; "
	                  "for f in /HBpersistence/core/" + app + ".* /mnt/persistence/" + app +
	                  ".* /tmp/" + app + ".*; do [ -f \"$f\" ] || continue; "
	                  "echo \"core file: $f\"; cp \"$f\" '" + var + "/' && rm -f \"$f\"; done; sync";
	pid_t pid = start({"/bin/ksh", "-c", cmd}, out_path, false);
	if (pid < 0) {
		log_msg("collect %s: spawn ksh: %s", app.c_str(), std::strerror(errno));
		return;
	}
	bool to, stop;
	await(pid, kPostS, to, stop);
}

// ---- the screen ----

bool screen_ok()
{
	if (!ui)
		return false;
	if (camera::active()) {
		if (!cam_was) {
			log_msg("screen: the camera is up, the menu stays away");
			screen_set_reshow(0);
			screen_give_back_to_camera();
			cam_was = true;
		}
		return false;
	}
	if (cam_was) {
		if (!camera::clear())
			return false;
		log_msg("screen: the camera is gone, the menu again");
		screen_show_again();
		screen_set_reshow(kReshowMs);
		cam_was = false;
		dirty = true;
	}
	return true;
}

void open_screen()
{
	ui = screen_open() == 0;
	if (!ui) {
		log_msg("FAIL runner: no screen");
		screen_close();
		return;
	}
	screen_set_reshow(kReshowMs);
	cam_was = false;
	dirty = true;
}

void close_screen()
{
	if (camera::active())
		screen_no_show_on_close();
	screen_close();
	ui = false;
}

u32 verdict_color(const std::string &v)
{
	if (v == "OK")
		return C_OK;
	if (v == "WARN" || v == "TIMEOUT")
		return C_WARN;
	return C_BAD;
}

struct Wrap {
	struct canvas *c;
	int y, max_y;
};

void wrap_line(const char *line, int nbytes, void *arg)
{
	Wrap *w = static_cast<Wrap *>(arg);
	if (w->y + font_small.h <= w->max_y)
		text_draw(w->c, &font_small, kMargin, w->y, line, nbytes, C_DIM);
	w->y += font_small.h + 3;
}

void draw(const runner::Menu &m)
{
	struct canvas *c = screen_canvas();
	char line[128];

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	text_draw(c, &font_big, kMargin, 8, "Test menu", -1, C_TITLE);
	text_draw(c, &font_small, c->w - kMargin - 37 * font_small.w, 18,
	          "main knob: turn = choose, press = run", -1, C_DIM);

	for (int r = 0; r < kRows && m.top() + r < m.count(); r++) {
		int i = m.top() + r, y = kListY + r * kRowH;
		const Item &it = items[i];
		bool sel = i == m.selected();
		if (sel)
			canvas_fill(c, kMargin / 2, y - 4, c->w - kMargin, kRowH, C_SEL);
		std::snprintf(line, sizeof(line), "%s %-.38s", sel ? ">" : " ",
		              it.app.name.empty() ? "Leave the menu" : it.app.title.c_str());
		text_draw(c, &font_big, kMargin, y, line, -1, sel ? C_TEXT : C_DIM);
		if (!it.verdict.empty())
			text_draw(c, &font_big, c->w - kMargin - 8 * font_big.w, y, it.verdict.c_str(), -1,
			          verdict_color(it.verdict));
	}
	if (m.top() > 0)
		text_draw(c, &font_small, c->w - kMargin - 3 * font_small.w, kListY - 18, "...", -1, C_DIM);
	if (m.top() + kRows < m.count())
		text_draw(c, &font_small, c->w - kMargin - 3 * font_small.w, kTextY - 18, "...", -1, C_DIM);

	const Item &sel = items[m.selected()];
	std::string text;
	if (sel.app.name.empty()) {
		text = "Give the screen back to the MMI and end the run.";
	} else {
		text = sel.app.name;
		if (!sel.verdict.empty())
			text += "  (last run: " + sel.verdict + ", " + sel.detail + ")";
		text += "\n" + sel.app.text;
	}
	Wrap w{c, kTextY, c->h - 36};
	text_wrap(text.c_str(), (c->w - 2 * kMargin) / font_small.w, wrap_line, &w);

	canvas_fill(c, 0, c->h - 32, c->w, 32, C_BAR);
	text_draw(c, &font_small, kMargin, c->h - 24, status.c_str(), -1, C_TEXT);
	screen_present();
	dirty = false;
}

// ---- one app ----

// Runs items[i] as run.sh would. false: we were asked to stop meanwhile.
bool run_item(Item &it)
{
	const runner::AppConf &a = it.app;
	std::string out_path = var + "/" + a.name + "_out.log";
	std::vector<std::string> args = {bin + "/" + a.name, sd};
	args.insert(args.end(), a.args.begin(), a.args.end());

	std::string cmdline;
	for (usize k = 1; k < args.size(); k++)
		cmdline += " " + args[k];
	log_msg("== start %s%s (timeout %u s)", a.name.c_str(), cmdline.c_str(), a.timeout_s);

	close_screen();
	unsigned t0 = log_ms();
	pid_t pid = start(args, out_path, true);
	bool timed_out = false, stopped = false;
	int rc = 127;
	if (pid < 0) {
		log_msg("FAIL spawn %s: %s", args[0].c_str(), std::strerror(errno));
		append_file(out_path, "spawn " + args[0] + ": " + std::strerror(errno) + "\n");
	} else {
		int st = await(pid, a.timeout_s, timed_out, stopped);
		rc = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;
	}
	unsigned secs = (log_ms() - t0 + 500) / 1000;
	unsigned fails = runner::count_fail_lines(read_file(out_path));
	const char *v = runner::verdict(pid >= 0, timed_out, rc, fails);

	char detail[96], lines[256];
	if (timed_out)
		std::snprintf(detail, sizeof(detail), "stopped after %u s", a.timeout_s);
	else if (pid < 0)
		std::snprintf(detail, sizeof(detail), "not started");
	else if (rc > 128)
		std::snprintf(detail, sizeof(detail), "signal %d, %u s", rc - 128, secs);
	else
		std::snprintf(detail, sizeof(detail), "exit %d, %u s", rc, secs);
	std::snprintf(lines, sizeof(lines),
	              "=== exit code: %d%s\n=== run time: %u s\n=== verdict: %s (%s, %u FAIL lines)\n",
	              rc, rc > 128 ? " (killed by a signal)" : "", secs, v, detail, fails);
	append_file(out_path, lines);
	std::snprintf(lines, sizeof(lines), "%s %s %s, %u FAIL lines\n", a.name.c_str(), v, detail, fails);
	append_file(var + "/summary.txt", lines);
	log_msg("%s: %s (%s), %u FAIL lines", a.name.c_str(), v, detail, fails);

	if (stopped)
		return false;
	collect(a.name, out_path);
	it.verdict = v;
	it.detail = detail;
	status = a.name + ": " + v + " (" + detail + ")";
	open_screen();
	return quit_poll() == QUIT_NONE;
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path>\n", argv[0]);
		return 2;
	}
	sd = argv[1];
	var = var_dir(argv[1]);
	std::string self = argv[0];
	usize slash = self.rfind('/');
	bin = slash == std::string::npos ? kDefaultBin : self.substr(0, slash);

	std::string logpath = var + "/runner.log";
	if (log_open(logpath.c_str()) != 0)
		std::fprintf(stderr, "runner: cannot open %s, logging to stdout only\n", logpath.c_str());
	log_msg("runner start, pid=%d, apps from %s", getpid(), bin.c_str());

	// quit.c reads ch8 for us and feeds the hub; no watchdog: the menu
	// stays until the card is pulled or "Leave the menu".
	// The hub lives as long as the process: quit.c's key thread keeps
	// calling into it until exit.
	buttons::Hub &hub = *new buttons::Hub;
	quit_init_ex(argv[1], 0, buttons::Hub::quit_cb, &hub);
	camera::start();

	for (const runner::AppConf &a : runner::parse_conf(read_file(sd + "/apps.conf"))) {
		if (a.name == kSelf)
			continue;
		if (!executable(bin + "/" + a.name))
			continue;
		items.push_back(Item{a, "", ""});
		log_msg("menu: %s (timeout %u s)", a.name.c_str(), a.timeout_s);
	}
	if (items.empty())
		status = "No apps on the card (bin/ + apps.conf). Press to leave.";
	items.push_back(Item{});    // "Leave the menu"

	runner::Menu menu((int)items.size(), kRows);
	buttons::EventQueue q(hub);
	open_screen();

	while (quit_poll() == QUIT_NONE) {
		buttons::Event e;
		bool start_now = false;
		bool got = q.wait(e, kRedrawMs);
		while (got) {
			runner::Menu::Act act = menu.on_event(e);
			if (e.action != buttons::Action::Touch)
				log_msg("key: %s", buttons::describe(e).c_str());
			if (act == runner::Menu::Act::Moved)
				dirty = true;
			if (act == runner::Menu::Act::Start) {
				start_now = true;
				break;
			}
			got = q.poll(e);    // the rest of a fast turn, then one redraw
		}
		if (start_now) {
			Item &it = items[menu.selected()];
			if (it.app.name.empty()) {
				log_msg("leave the menu");
				break;
			}
			if (!run_item(it))
				break;
			q.clear();          // presses made during the app are not for us
			continue;
		}
		if (screen_ok() && dirty)
			draw(menu);
	}
	quit_request(QUIT_TIMER);

	close_screen();
	camera::stop();
	log_msg("runner exit (stop: %s), %u ch8 messages", quit_name(quit_poll()), hub.messages());
	log_close();
	return 0;
}

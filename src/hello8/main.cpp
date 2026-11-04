// hello8 - C++ on the MMI: the first app built with shle-nto-g++ (GCC 16,
// libstdc++ linked statically, -std=gnu++20; sdk/include-c++/README.md).
//
// Runs checks.cpp's checks one after the other - language runtime (static
// init, exceptions, RTTI, new/delete) and standard library (containers,
// algorithms, strings, functional, smart pointers, math, random, chrono,
// threads, condition variables, atomics) - and shows them on a CPU-drawn
// screen as they finish: PASS in green, FAIL in red with the expectation
// that failed. The results stay up for <seconds> after the last one.
//
// Each check's name goes to the log before it runs, so if one crashes or
// hangs the unit the last "check ..." line names it. A static object's
// destructor appends a line after main() returns: the at-exit path.
//
// Output: <sd>/hello8.log: the toolchain (GCC, C++ standard, type sizes),
// one PASS/FAIL line per check with its time and notes, RESULT counts.
//
// Stops after <seconds>, on SIGTERM/INT/HUP, SD removal or the watchdog
// (src/common/quit.c).
//
// usage: hello8 <sd_path> [seconds] [check,check,...]
//        seconds: how long the results stay up (default 20)
//        checks: run only these (names as on screen, e.g. "map/set,threads")
extern "C" {
#include "font_big.h"
#include "font_small.h"
#include "log.h"
#include "quit.h"
#include "screen.h"
#include "text.h"
}

#include "checks.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr unsigned kDefaultSeconds = 20;
constexpr unsigned kWatchdogSlack = 60;    // the checks run before the countdown
constexpr int kMargin = 20;

constexpr uint32_t C_BG = 0xff101820u;
constexpr uint32_t C_TEXT = 0xffe8e8e8u;
constexpr uint32_t C_DIM = 0xff8a929eu;
constexpr uint32_t C_TITLE = 0xffffd040u;
constexpr uint32_t C_PASS = 0xff40d060u;
constexpr uint32_t C_FAIL = 0xffff5050u;
constexpr uint32_t C_RUN = 0xffffb020u;
constexpr uint32_t C_BAR = 0xff1c2836u;

char exit_log_path[256];

// Destroyed after main() returns, through __cxa_atexit: the log is closed
// by then, so it appends on its own.
struct ExitProbe {
	~ExitProbe()
	{
		std::printf("hello8: static destructor ran at exit\n");
		if (!exit_log_path[0])
			return;
		if (FILE *f = std::fopen(exit_log_path, "a")) {
			std::fputs("static destructor ran at exit\n", f);
			std::fclose(f);
		}
	}
} exit_probe;

struct Row {
	const checks::Check *check;
	enum { WAITING, RUNNING, DONE } state = WAITING;
	checks::Result result;
};

std::vector<Row> rows;

void draw(const std::string &status)
{
	struct canvas *c = screen_canvas();
	const int nrows = ((int)rows.size() + 1) / 2;
	const int col_w = (c->w - 2 * kMargin) / 2;
	const int cols = col_w / font_small.w - 2;          // characters per detail line
	const int row_h = 2 * font_small.h + 6;
	char line[160];

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	text_draw(c, &font_big, kMargin, 8, "hello8  C++ on the MMI", -1, C_TITLE);
	std::snprintf(line, sizeof(line), "t = %u ms", log_ms());
	text_draw(c, &font_big, c->w - kMargin - (int)std::strlen(line) * font_big.w, 8,
	          line, -1, C_TEXT);
	std::snprintf(line, sizeof(line), "GCC %s, libstdc++ static, C++%ld",
	              __VERSION__, __cplusplus / 100 % 100);
	text_draw(c, &font_small, kMargin, 52, line, -1, C_DIM);

	for (size_t i = 0; i < rows.size(); i++) {
		const Row &row = rows[i];
		int x = kMargin + (int)(i / nrows) * col_w;
		int y = 80 + (int)(i % nrows) * row_h;
		const char *tag = "    ";
		uint32_t color = C_DIM;
		std::string detail;

		if (row.state == Row::RUNNING) {
			tag = "RUN ";
			color = C_RUN;
		} else if (row.state == Row::DONE) {
			tag = row.result.ok ? "PASS" : "FAIL";
			color = row.result.ok ? C_PASS : C_FAIL;
			detail = row.result.detail;
		}
		if (row.state == Row::DONE)
			std::snprintf(line, sizeof(line), "%s  %-15s %6u ms", tag, row.check->name,
			              row.result.ms);
		else
			std::snprintf(line, sizeof(line), "%s  %s", tag, row.check->name);
		text_draw(c, &font_small, x, y, line, -1, color);
		if ((int)detail.size() > cols)
			detail = detail.substr(0, cols - 3) + "...";
		text_draw(c, &font_small, x + 6 * font_small.w, y + font_small.h + 2,
		          detail.c_str(), -1, row.result.ok ? C_DIM : C_TEXT);
	}

	canvas_fill(c, 0, c->h - 32, c->w, 32, C_BAR);
	text_draw(c, &font_small, kMargin, c->h - 24, status.c_str(), -1, C_TEXT);
	screen_present();
}

void log_toolchain()
{
	log_msg("built with GCC %s, __cplusplus %ld, libstdc++ %d (_GLIBCXX_RELEASE %d)",
	        __VERSION__, __cplusplus, __GLIBCXX__, _GLIBCXX_RELEASE);
#if defined(__SH_ATOMIC_MODEL_HARD_LLCS__)
	log_msg("atomics: hard-llcs (SH-4A movli.l/movco.l)");
#elif defined(__SH_ATOMIC_MODEL_SOFT_GUSA__)
	log_msg("atomics: soft-gusa");
#endif
	log_msg("sizes: std::string %u, std::mutex %u (pthread_mutex_t %u), "
	        "std::condition_variable %u (pthread_cond_t %u), std::thread %u, "
	        "std::shared_ptr %u, wchar_t %u, long double %u, time_t %u",
	        (unsigned)sizeof(std::string), (unsigned)sizeof(std::mutex),
	        (unsigned)sizeof(pthread_mutex_t), (unsigned)sizeof(std::condition_variable),
	        (unsigned)sizeof(pthread_cond_t), (unsigned)sizeof(std::thread),
	        (unsigned)sizeof(std::shared_ptr<int>), (unsigned)sizeof(wchar_t),
	        (unsigned)sizeof(long double), (unsigned)sizeof(time_t));
}

bool selected(const char *only, const char *name)
{
	if (!only)
		return true;
	std::string list = std::string(",") + only + ",";
	return list.find(std::string(",") + name + ",") != std::string::npos;
}

void nap(unsigned ms)
{
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};

	nanosleep(&ts, nullptr);
}

} // namespace

int main(int argc, char *argv[])
{
	checks::mark_main();

	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path> [seconds] [check,check,...]\n", argv[0]);
		return 2;
	}
	unsigned seconds = kDefaultSeconds;
	if (argc > 2 && std::atoi(argv[2]) > 0)
		seconds = (unsigned)std::atoi(argv[2]);
	const char *only = argc > 3 ? argv[3] : nullptr;

	char logpath[256];
	std::snprintf(logpath, sizeof(logpath), "%s/hello8.log", argv[1]);
	if (log_open(logpath) != 0)
		std::fprintf(stderr, "hello8: cannot open %s, logging to stdout only\n", logpath);
	else
		std::snprintf(exit_log_path, sizeof(exit_log_path), "%s", logpath);
	log_msg("hello8 start, pid=%d, results shown %u s%s%s", getpid(), seconds,
	        only ? ", only: " : "", only ? only : "");
	log_toolchain();

	quit_init(argv[1], seconds + kWatchdogSlack);
	bool ui = screen_open() == 0;
	if (!ui)
		log_msg("FAIL hello8: no screen, logging only");

	for (const checks::Check &c : checks::all())
		if (selected(only, c.name))
			rows.push_back(Row{&c});

	int passed = 0, failed = 0;
	for (Row &row : rows) {
		if (quit_poll() != QUIT_NONE)
			break;
		row.state = Row::RUNNING;
		if (ui)
			draw(std::string("running: ") + row.check->name);
		log_msg("check %s ...", row.check->name);
		row.result = checks::run(*row.check);
		row.state = Row::DONE;
		if (row.result.ok) {
			passed++;
			log_msg("PASS %s (%u ms)%s%s", row.check->name, row.result.ms,
			        row.result.detail.empty() ? "" : ": ", row.result.detail.c_str());
		} else {
			failed++;
			log_msg("FAIL %s (%u ms): %s", row.check->name, row.result.ms,
			        row.result.detail.c_str());
		}
	}
	log_msg("RESULT %d of %d checks passed%s", passed, (int)rows.size(),
	        passed + failed < (int)rows.size() ? " (stopped early)" : "");

	char status[96];
	unsigned end = log_ms() + seconds * 1000, shown_left = 0;
	while (log_ms() < end && quit_poll() == QUIT_NONE) {
		unsigned left = (end - log_ms() + 999) / 1000;
		if (ui && left != shown_left) {
			std::snprintf(status, sizeof(status), "%d of %d passed, %d failed.  ends in %u s",
			              passed, (int)rows.size(), failed, left);
			draw(status);
			shown_left = left;
		}
		nap(100);
	}
	quit_request(QUIT_TIMER);

	screen_close();
	log_msg("hello8 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

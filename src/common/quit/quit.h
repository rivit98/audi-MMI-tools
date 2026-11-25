// quit.h - how a long-running app is asked to stop, and its cleanup.
//
// The app fills a quit::Config with the stop sources it wants and calls
// quit::start() once, after log_open() and before any other thread:
//   Timer     run_ms is over
//   Signal    SIGTERM / SIGINT / SIGHUP (`slay hello3`, the runner): always on
//   SdCard    sd_path is gone: the card was pulled (checked once a second,
//             in poll)
//   Key       stop_key (a ch8.h button) was pressed; only then is ch8
//             subscribed (ipc.h)
//   Watchdog  the app didn't stop by itself within watchdog_s
//   Done      the app finished (finish() with no stop asked for)
// The app may also ask itself: quit::request().
//
// The main thread polls quit::stopping() and, on a stop, leaves its loop and
// calls quit::finish(): that runs cleanup once, on the main thread (GL, gf
// and the layer manager are not thread-safe, so cleanup never runs beside a
// main thread that is still drawing).
//
// Hard path: kHardS seconds after a stop request, or on a second signal, the
// process ends with _exit(). If cleanup hasn't started by then (the main
// thread hung in GL, say), it runs first, on the signal thread, with
// hard = true: do only what must not be left behind (give the screen back)
// and expect anything the main thread holds to be stuck. A cleanup that
// hangs there is cut short after kHardCleanupS.
//
// Signals are taken by a sigwait() thread, never by a handler, so the GL/gf
// calls in the main thread are not interrupted (no EINTR).
//
// Short tests that hold nothing (no screen, no graph) don't need any of
// this: they run and return.
#ifndef COMMON_QUIT_QUIT_H
#define COMMON_QUIT_QUIT_H

#include "types.h"

#include <functional>

namespace quit {

enum class Reason { None, Done, Timer, Signal, SdCard, Key, Watchdog };

constexpr unsigned kHardS = 10;
constexpr unsigned kHardCleanupS = 3;

struct Config {
	const char *sd_path = nullptr;  // stop when it's gone; nullptr: not watched
	unsigned run_ms = 0;            // stop after this long; 0: no timer
	unsigned watchdog_s = 0;        // stop if still running then, hard exit after; 0: none
	int stop_key = 0;               // stop on this ch8 button press (ch8.h CH8_*); 0: none
	// Undo what the app holds (screen, io-media graph, videoctrl). hard: see
	// above. Empty: nothing to undo.
	std::function<void(bool hard)> cleanup;
};

// false if the signal thread could not be started (then signals and the
// watchdog kill without cleanup).
bool start(const Config &c);

// Ask for a stop; the first reason wins and is logged.
void request(Reason r);

// Reason::None while the app should keep running, else the reason.
Reason poll();
inline bool stopping() { return poll() != Reason::None; }

// The end of the run, on the main thread: Done unless a stop was asked for,
// then cleanup once, bounded by the hard path. Returns the reason.
Reason finish();

const char *name(Reason r);

} // namespace quit

#endif

/*
 * quit.h - how a long-running app is asked to stop, and its cleanup.
 *
 * The app fills a quit_config with the stop sources it wants and calls
 * quit_start() once, after log_open() and before any other thread:
 *   QUIT_TIMER     run_ms is over
 *   QUIT_SIGNAL    SIGTERM / SIGINT / SIGHUP (`slay hello3`, the runner):
 *                  always on
 *   QUIT_SDCARD    sd_path is gone: the card was pulled (checked once a
 *                  second, in quit_poll)
 *   QUIT_KEY       stop_key (a ch8.h button) was pressed; only then is ch8
 *                  subscribed (ipc.h)
 *   QUIT_WATCHDOG  the app didn't stop by itself within watchdog_s
 *   QUIT_DONE      the app finished (quit_finish with no stop asked for)
 * The app may also ask itself: quit_request().
 *
 * The main thread polls quit_poll() and, on a stop, leaves its loop and
 * calls quit_finish(): that runs cleanup() once, on the main thread (GL, gf
 * and the layer manager are not thread-safe, so cleanup never runs beside a
 * main thread that is still drawing).
 *
 * Hard path: QUIT_HARD_S seconds after a stop request, or on a second
 * signal, the process ends with _exit(). If cleanup() hasn't started by
 * then (the main thread hung in GL, say), it runs first, on the signal
 * thread, with hard = 1: do only what must not be left behind (give the
 * screen back) and expect anything the main thread holds to be stuck. A
 * cleanup that hangs there is cut short after QUIT_HARD_CLEANUP_S.
 *
 * Signals are taken by a sigwait() thread, never by a handler, so the
 * GL/gf calls in the main thread are not interrupted (no EINTR).
 *
 * Short tests that hold nothing (no screen, no graph) don't need any of
 * this: they run and return.
 */
#ifndef COMMON_QUIT_QUIT_H
#define COMMON_QUIT_QUIT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	QUIT_NONE = 0,
	QUIT_DONE,
	QUIT_TIMER,
	QUIT_SIGNAL,
	QUIT_SDCARD,
	QUIT_KEY,
	QUIT_WATCHDOG,
};

#define QUIT_HARD_S          10
#define QUIT_HARD_CLEANUP_S  3

struct quit_config {
	const char *sd_path;    /* stop when it's gone; NULL: not watched */
	unsigned run_ms;        /* stop after this long; 0: no timer */
	unsigned watchdog_s;    /* stop if still running then, hard exit after; 0: none */
	int stop_key;           /* stop on this ch8 button press (ch8.h CH8_*); 0: none */
	/* Undo what the app holds (screen, io-media graph, videoctrl). hard:
	 * see above. NULL: nothing to undo. */
	void (*cleanup)(int hard, void *arg);
	void *arg;
};

/* Returns 0, or -1 if the signal thread could not be started (then signals
 * and the watchdog kill without cleanup). */
int  quit_start(const struct quit_config *c);

/* Ask for a stop; the first reason wins and is logged. */
void quit_request(int reason);

/* QUIT_NONE while the app should keep running, else the reason. */
int  quit_poll(void);

/* The end of the run, on the main thread: QUIT_DONE unless a stop was
 * asked for, then cleanup() once, bounded by the hard path. Returns the
 * reason. */
int  quit_finish(void);

const char *quit_name(int reason);

#ifdef __cplusplus
}
#endif

#endif

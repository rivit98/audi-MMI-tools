/*
 * quit.h - every way a long-running app can be asked to stop cleanly.
 *
 * The app polls quit_poll() once per frame and, when it returns non-zero,
 * leaves its loop and runs its normal teardown (gfx_shutdown). Sources:
 *   QUIT_TIMER     the app's own run time is over (it calls quit_request)
 *   QUIT_KEY       a hardware button press on /dev/ipc/ch8 (qnx-audi's
 *                  rule: any message that is not a knob turn)
 *   QUIT_SIGNAL    SIGTERM / SIGINT / SIGHUP (`slay hello3` from a shell)
 *   QUIT_SDCARD    the SD card was pulled (checked once per second)
 *   QUIT_WATCHDOG  the app did not stop by itself within watchdog_s
 * After any stop request the app has QUIT_HARD_S seconds to exit. If it is
 * still running then (GL or the layer manager hung), or on a second
 * SIGTERM/SIGINT, the process is ended with _exit().
 *
 * Signals are taken by a sigwait() thread, never by a handler, so the
 * GL/gf calls in the main thread are not interrupted (no EINTR).
 */
#ifndef RIVIT_QUIT_H
#define RIVIT_QUIT_H

enum {
	QUIT_NONE = 0,
	QUIT_TIMER,
	QUIT_KEY,
	QUIT_SIGNAL,
	QUIT_SDCARD,
	QUIT_WATCHDOG,
};

#define QUIT_HARD_S  10

/* Called for every raw /dev/ipc/ch8 message, on the key thread, before the
 * built-in button rule runs - so an app can react to knob turns (which the
 * button rule ignores) without opening ch8 a second time. buf holds n bytes
 * (byte 7/6 = rotary knob left/right, 5/4 = volume knob; all of 4..7 zero is
 * a button). May be NULL. */
typedef void (*quit_key_cb)(const unsigned char *buf, int n, void *arg);

/* Call after log_open() and before any other thread exists. sd_path is
 * watched for card removal; watchdog_s arms alarm(). Returns 0, or -1 if
 * the signal thread could not be started (then only timer/SD/key work). */
int  quit_init(const char *sd_path, unsigned watchdog_s);

/* As quit_init(), plus a key observer. quit_init() is this with cb=NULL. */
int  quit_init_ex(const char *sd_path, unsigned watchdog_s,
                  quit_key_cb cb, void *cb_arg);

/* Ask for a stop; the first reason wins and is logged. */
void quit_request(int reason);

/* QUIT_NONE while the app should keep running, else the reason. */
int  quit_poll(void);

const char *quit_name(int reason);

#endif

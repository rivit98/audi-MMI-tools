/*
 * ducklink.h - hello5 step d's helper process, seen from hello5.
 *
 * A MsgSend to the wrong channel of MMI3GApplication can block the sender
 * for good: no timeout, signal, _exit() or SIGKILL ends it (docs/hbdsi.md
 * section 6.1). So hello5 sends nothing to the MMI itself. It spawns a
 * copy of itself without a screen ("hello5 <sd_path> @duck", duckhelper.c)
 * that does every HBDSI send, and talks to it over two pipes, one line at
 * a time, with a deadline on every answer. A helper that misses one is
 * killed and left behind; hello5 carries on and exits as usual.
 *
 * Protocol (text lines):
 *   hello5 -> helper, its stdin:  a command ("connect", "focus 14 1", "rx",
 *                                 "quit"); EOF = quit
 *   helper -> hello5, its fd 3:   "wait <ms> <what>"  about to block up to
 *                                                     ms in <what>
 *                                 "note <text>"       a status line for the
 *                                                     screen
 *                                 "ok <text>"         the command's answer
 *                                 "err <text>"        the command failed
 * Each "wait" moves the deadline to now + ms + DUCK_LINK_SLACK_MS. The
 * helper's stdout and stderr go to /dev/null; it logs to its own file.
 */
#ifndef HELLO5_DUCKLINK_H
#define HELLO5_DUCKLINK_H

#include <sys/types.h>

#include "types.h"

#define DUCK_LINK_SLACK_MS 2000     /* added to every "wait" */
#define DUCK_LINK_FD       3        /* the helper's answer fd */
#define DUCK_LINK_POLL_MS  100      /* how often stop() is asked */

/* The answers of link_cmd. */
enum {
	LINK_OK = 0,        /* "ok", text in reply */
	LINK_ERR,           /* "err", text in reply */
	LINK_TIMEOUT,       /* no answer by the deadline: helper killed, given up */
	LINK_GONE,          /* the helper exited or was never started */
	LINK_STOP           /* stop() said stop: nothing is known */
};

struct duck_link {
	pid_t pid;          /* the helper, 0 = none */
	int to;             /* its stdin */
	int from;           /* its fd 3 */
	int dead;           /* given up on: no more commands */
	char buf[512];      /* a partial line from the helper */
	usize len;
	char note[96];      /* the last "note" line */
};

void link_init(struct duck_link *l);

/* Spawn argv (argv[0] a path) as the helper. 0, or an errno. */
int link_start(struct duck_link *l, char *const argv[]);

/* Send cmd (no newline) and wait for its "ok"/"err". first_ms: the deadline
 * until the helper's first "wait". stop: polled every DUCK_LINK_POLL_MS,
 * non-zero = give up (NULL: never). reply gets the text after ok/err.
 * Returns one of LINK_*. */
int link_cmd(struct duck_link *l, const char *cmd, unsigned first_ms,
             char *reply, usize size, int (*stop)(void));

/* "quit" (if still alive), then up to grace_ms for it to exit, then
 * SIGKILL and another short wait. Never blocks longer than that: a helper
 * that does not exit is logged and left behind. Returns 1 if it exited
 * (or there was none), 0 if it was left behind. */
int link_end(struct duck_link *l, unsigned grace_ms);

/* One helper line into its parts: the keyword's LINK_OK / LINK_ERR, -1
 * for "wait" (*ms set), -2 for "note", -3 for anything else. *rest points
 * after the keyword and its space. Exposed for the tests. */
int link_parse(const char *line, unsigned *ms, const char **rest);

#endif

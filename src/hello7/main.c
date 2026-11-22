/*
 * hello7 - knob, button, steering wheel and touch pad tester: every
 * /dev/ipc/ch8 message, decoded (keys.c: panel, tpad.c: wheel and pad),
 * shown live and logged.
 *
 * Two modes:
 *
 *   guide (default)  one open question at a time (guide.c): the steering
 *                    wheel's volume roller, then the touch pad in the
 *                    current screen, in every recognizer mode (hello7
 *                    writes the HMI's "set mode" command to ch8) and on the
 *                    HMI's radio, map and speller screens. A prompt moves
 *                    on a few seconds after the last message it wants (15 s
 *                    with nothing: the car has no such control). The
 *                    summary names what each prompt got and flags two
 *                    prompts that got the same.
 *   free <seconds>   no prompts: turn and press anything. A counter per
 *                    known control (the last one in yellow) and the latest
 *                    messages with their first 16 bytes. A bit no known
 *                    control explains shows as "unknown b<byte> 0x<bits>".
 *
 * The HMI still acts on every knob and button (docs/TODO.md): MENU, BACK
 * and the source keys bring its own screens up, so ours is shown again
 * every second. While the HMI shows the rear camera ours stays away.
 *
 * Output: <sd>/var/hello7.log, every message in full (27 bytes) with its
 * decoding, the prompt it belongs to and the time since the previous one;
 * every prompt's start and result; RESULT lines.
 *
 * Stops at the end, on SIGTERM/INT/HUP, SD removal or the watchdog
 * (src/common/quit.c). No key or button stops it.
 *
 * usage: hello7 <sd_path> [guide | <seconds>]   (default guide; a number:
 *        free mode for that long)
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cam.h"
#include "font_big.h"
#include "font_small.h"
#include "guide.h"
#include "keys.h"
#include "log.h"
#include "quit.h"
#include "screen.h"
#include "text.h"
#include "tpad.h"
#include "types.h"

#define WATCHDOG_SLACK  20
#define REDRAW_MS       100
#define RESHOW_MS       1000
#define SUMMARY_MS      45000
#define RECENT          8       /* messages kept on screen */
#define MARGIN          20
#define KEY_DEVICE      "/dev/ipc/ch8"
#define HEX_MAX         40      /* bytes logged per message */

#define C_BG     0xff101820u
#define C_TEXT   0xffe8e8e8u
#define C_DIM    0xff8a929eu
#define C_TITLE  0xffffd040u
#define C_HOT    0xffffb020u
#define C_OK     0xff60d060u
#define C_BAD    0xffff6060u
#define C_BAR    0xff1c2836u

/* The counters on screen: the named controls, then unknown bits. */
static const int shown[] = {
	K_MAIN_RIGHT, K_MAIN_LEFT, K_VOL_UP, K_VOL_DOWN, K_MENU,
	K_SOFT_TL, K_B1_40, K_B1_80, K_UNKNOWN,
};
#define NSHOWN ((int)(sizeof(shown) / sizeof(shown[0])))

/* Written by on_key (quit.c's key thread), read by the main loop. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned counts[K_NCTL];         /* detents for knobs, presses else */
static unsigned msgs, last_ms;
static int last_ctl = -1;
static char recent[RECENT][112];
static unsigned nrecent;                /* lines ever added */
static char last_what[96];              /* the latest message, decoded */
static int dirty = 1;
static int guided;
static struct guide g;
static int last_echo = -1;              /* the last pad mode the pad echoed */

static void on_key(const u8 *buf, int n, void *arg)
{
	struct key_hit hits[KEY_MAX_HITS];
	char what[96], hex[HEX_MAX * 3 + 1], gap_s[16], where[48] = "";
	unsigned now = log_ms(), gap;
	int nh, i;

	(void)arg;
	nh = key_decode(buf, n, hits, KEY_MAX_HITS);
	key_describe(hits, nh, what, sizeof(what));
	if (tpad_sub(buf, n) >= 0) {
		tpad_describe(buf, n, what, sizeof(what));
		nh = 0;                         /* not a panel release */
	} else {
		char wheel[96];

		mfl_describe(buf, n, wheel, sizeof(wheel));
		if (wheel[0]) {
			snprintf(what, sizeof(what), "%s", wheel);
			nh = 0;
		}
	}
	for (i = 0; i < n && i < HEX_MAX; i++)
		snprintf(hex + i * 3, 4, "%02x ", buf[i]);
	hex[i * 3] = '\0';

	pthread_mutex_lock(&lock);
	gap = msgs ? now - last_ms : 0;
	if (tpad_mode_echo(buf, n) >= 0)
		last_echo = tpad_mode_echo(buf, n);
	msgs++;
	last_ms = now;
	for (i = 0; i < nh; i++) {
		counts[hits[i].ctl] += (unsigned)hits[i].amount;
		if (hits[i].ctl != K_RELEASE)
			last_ctl = hits[i].ctl;
	}
	if (guided && !guide_done(&g)) {
		guide_key(&g, buf, n, now);
		snprintf(where, sizeof(where), " [%d %s]", g.cur + 1, guide_prompts[g.cur].name);
	}
	if (nh != 1 || hits[0].ctl != K_RELEASE)
		snprintf(last_what, sizeof(last_what), "%s", what);
	else if (n >= 27 && buf[16] == 3)
		snprintf(what, sizeof(what), "wheel release");
	/* the screen shows the first 16 bytes, the log all of them */
	snprintf(gap_s, sizeof(gap_s), "+%u ms", gap);
	snprintf(recent[nrecent % RECENT], sizeof(recent[0]),
	         "#%-4u %-10s %-26.26s %.47s", msgs, gap_s, what, hex);
	nrecent++;
	dirty = 1;
	pthread_mutex_unlock(&lock);

	log_msg("key #%u (+%u ms)%s: %s; %d bytes: %s", msgs, gap, where, what, n, hex);
}

static void nap(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Set the pad's recognizer mode the way the HMI does (MMI3GApplication
 * 0x82288B8 -> 0x8228628): open ch8, write the 10-byte command, close. The
 * pad echoes the mode back (subfunction C4). quit.c keeps reading ch8 on
 * its own fd; this one is open for the write only, so it queues nothing. */
static int pad_set_mode(int mode)
{
	u8 cmd[16];
	int len = tpad_mode_cmd(mode, cmd, sizeof(cmd)), fd, w, err;

	fd = open(KEY_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		log_msg("pad mode %d: open %s failed: %s", mode, KEY_DEVICE, strerror(errno));
		return -1;
	}
	w = (int)write(fd, cmd, (usize)len);
	err = errno;
	close(fd);
	if (w != len) {
		log_msg("pad mode %d: write returned %d (%s)", mode, w, w < 0 ? strerror(err) : "short");
		return -1;
	}
	log_msg("pad mode %d: sent 22 00 00 00 00 00 02 00 c4 %02x", mode, mode);
	return 0;
}

/* ---- the screen, out of the rear camera's way ---- */

static int ui;                          /* our displayable is up */
static int cam_was;
static int hmi_step;                    /* a step on the HMI's screens: no re-show */

/* 1 if we may draw now. While the HMI shows the camera: no drawing and no
 * re-show; after it, ours comes back once the HMI has shown its own. */
static int screen_ok(void)
{
	if (!ui)
		return 0;
	if (cam_active()) {
		if (!cam_was) {
			log_msg("screen: the camera is up, ours stays away");
			screen_set_reshow(0);
			screen_give_back_to_camera();
			cam_was = 1;
		}
		return 0;
	}
	if (cam_was) {
		if (!cam_clear())
			return 0;
		log_msg("screen: the camera is gone, ours again");
		if (!hmi_step) {
			screen_show_again();
			screen_set_reshow(RESHOW_MS);
		}
		cam_was = 0;
		dirty = 1;
	}
	return 1;
}

struct wrap {
	struct canvas *c;
	const struct font *f;
	int y;
	u32 color;
};

static void wrap_line(const char *line, int nbytes, void *arg)
{
	struct wrap *w = arg;

	text_draw(w->c, w->f, MARGIN, w->y, line, nbytes, w->color);
	w->y += w->f->h + 4;
}

/* Word-wrapped; returns the y under it. */
static int draw_wrapped(struct canvas *c, const struct font *f, int y, const char *s, u32 color)
{
	struct wrap w = { c, f, y, color };

	text_wrap(s, (c->w - 2 * MARGIN) / f->w, wrap_line, &w);
	return w.y;
}

/* The latest messages, from y down. Under the lock. */
static void draw_recent(struct canvas *c, int y)
{
	unsigned i, n = nrecent < RECENT ? nrecent : RECENT, first = nrecent - n;

	for (i = 0; i < n && y + font_small.h <= c->h - 34; i++, y += font_small.h + 2)
		text_draw(c, &font_small, MARGIN, y, recent[(first + i) % RECENT], -1,
		          i + 1 == n ? C_TEXT : C_DIM);
}

static void draw_bar(struct canvas *c, const char *text)
{
	canvas_fill(c, 0, c->h - 32, c->w, 32, C_BAR);
	text_draw(c, &font_small, MARGIN, c->h - 24, text, -1, C_TEXT);
}

static void draw_free(unsigned left_s)
{
	struct canvas *c = screen_canvas();
	char line[64];
	int i, x, y;

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	text_draw(c, &font_big, MARGIN, 8, "hello7  knobs and buttons", -1, C_TITLE);
	text_draw(c, &font_small, MARGIN, 52,
	          "Turn any knob, press any button: each message shows up here.", -1, C_DIM);

	pthread_mutex_lock(&lock);
	/* two columns of counters */
	for (i = 0; i < NSHOWN; i++) {
		x = MARGIN + (i % 2) * 380;
		y = 80 + (i / 2) * (font_big.h + 4);
		snprintf(line, sizeof(line), "%-17s %4u", key_name(shown[i]), counts[shown[i]]);
		text_draw(c, &font_big, x, y, line, -1, shown[i] == last_ctl ? C_HOT : C_TEXT);
	}
	snprintf(line, sizeof(line), "%u messages", msgs);
	text_draw(c, &font_small, MARGIN, 262, line, -1, C_DIM);
	draw_recent(c, 284);
	dirty = 0;
	pthread_mutex_unlock(&lock);

	snprintf(line, sizeof(line), "ends in %u s", left_s);
	draw_bar(c, line);
	screen_present();
}

static void draw_guide(void)
{
	struct canvas *c = screen_canvas();
	const struct guide_prompt *p;
	const struct guide_result *r;
	char line[300], sig[160];
	unsigned left;
	int y;

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	pthread_mutex_lock(&lock);
	p = &guide_prompts[g.cur];
	r = &g.res[g.cur];
	snprintf(line, sizeof(line), "hello7  step %d of %d", g.cur + 1, guide_nprompts);
	text_draw(c, &font_big, MARGIN, 8, line, -1, C_TITLE);
	y = draw_wrapped(c, &font_big, 56, p->text, C_TEXT);
	if (p->hint[0])
		y = draw_wrapped(c, &font_small, y + 4, p->hint, C_DIM);
	y += 12;
	if (r->seen) {
		guide_signature(r, sig, sizeof(sig));
		snprintf(line, sizeof(line), "Got: %s  (%s)", sig, last_what);
		y = draw_wrapped(c, &font_small, y, line, C_OK);
	} else {
		y = draw_wrapped(c, &font_small, y, "Waiting for it ...", C_HOT);
	}
	draw_recent(c, y + 12);
	left = guide_left_ms(&g, log_ms());
	dirty = 0;
	snprintf(line, sizeof(line), r->seen ? "next step in %u s (when you stop)"
	                                     : "skipped in %u s if nothing comes",
	         (left + 999) / 1000);
	pthread_mutex_unlock(&lock);
	draw_bar(c, line);
	screen_present();
}

/* One line per prompt, two columns. */
static void draw_summary(void)
{
	struct canvas *c = screen_canvas();
	char line[96], sig[64];
	int i, half = (guide_nprompts + 1) / 2, x, y, same;

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	text_draw(c, &font_big, MARGIN, 8, "hello7 results (also in hello7.log)", -1, C_TITLE);
	for (i = 0; i < guide_nprompts; i++) {
		const struct guide_result *r = &g.res[i];

		x = MARGIN + (i / half) * 390;
		y = 52 + (i % half) * (font_small.h + 7);
		guide_signature(r, sig, sizeof(sig));
		same = guide_same_as(&g, i);
		snprintf(line, sizeof(line), "%-21.21s %s%s", guide_prompts[i].name,
		         same >= 0 ? "SAME " : "", r->seen ? sig : "-");
		text_draw(c, &font_small, x, y, line, -1,
		          !r->seen ? C_DIM : same >= 0 ? C_BAD : C_TEXT);
	}
	draw_bar(c, "SAME = got the same bytes as an earlier step. - = nothing came.");
	screen_present();
}

/* ---- the two modes ---- */

static void log_result(int i)
{
	const struct guide_result *r = &g.res[i];
	char sig[160], presses[96];
	int same = guide_same_as(&g, i);

	guide_signature(r, sig, sizeof(sig));
	guide_press_list(r, presses, sizeof(presses));
	log_msg("step %d %s: %s; %u messages, %u it wants, %u ms%s%s%s%s%s", i + 1,
	        guide_prompts[i].name, sig, r->msgs, r->presses, r->end_ms - r->begin_ms,
	        r->idle ? ", nothing came" : r->capped ? ", ended by the time limit" : "",
	        same >= 0 ? ", SAME as " : "", same >= 0 ? guide_prompts[same].name : "",
	        presses[0] ? "; finger down at " : "", presses);
}

/* forced: hello7 set a pad mode and no HMI screen has set its own since */
static int forced = -1, mode_before = -1;

static void start_prompt(void)
{
	const struct guide_prompt *p = &guide_prompts[g.cur];

	log_msg("== step %d/%d: %s: \"%s\"", g.cur + 1, guide_nprompts, p->name, p->text);
	if (p->set_mode > 0) {
		if (forced < 0) {
			pthread_mutex_lock(&lock);
			mode_before = last_echo;
			pthread_mutex_unlock(&lock);
		}
		pad_set_mode(p->set_mode);
		forced = p->set_mode;
	}
	if (p->hmi) {
		/* the HMI's screens stay up while this step runs; its screen
		 * change sets the pad's mode again */
		hmi_step = 1;
		forced = -1;
		screen_set_reshow(0);
	} else if (hmi_step) {
		hmi_step = 0;
		if (ui && !cam_active()) {
			screen_show_again();
			screen_set_reshow(RESHOW_MS);
		}
	}
}

static void run_guide(void)
{
	unsigned shown_left = 0, left, end;
	int ended, i, seen;

	pthread_mutex_lock(&lock);
	guide_start(&g, log_ms());
	guided = 1;
	pthread_mutex_unlock(&lock);
	start_prompt();
	while (quit_poll() == QUIT_NONE) {
		pthread_mutex_lock(&lock);
		ended = guide_tick(&g, log_ms());
		pthread_mutex_unlock(&lock);
		if (ended) {
			log_result(g.cur - 1);
			if (guide_done(&g))
				break;
			start_prompt();
			dirty = 1;
		}
		left = guide_left_ms(&g, log_ms()) / 1000;
		if (screen_ok() && (dirty || left != shown_left)) {
			draw_guide();
			shown_left = left;
		}
		nap(REDRAW_MS);
	}
	if (hmi_step) {                 /* ours again for the summary */
		hmi_step = 0;
		if (ui && !cam_active()) {
			screen_show_again();
			screen_set_reshow(RESHOW_MS);
		}
	}
	if (forced >= 0) {
		if (mode_before >= 0) {
			log_msg("pad: back to mode %d, the one it had before hello7 set any", mode_before);
			pad_set_mode(mode_before);
		} else {
			log_msg("pad: left in mode %d; the HMI sets its own on the next screen change",
			        forced);
		}
	}

	for (i = 0, seen = 0; i < g.cur; i++)
		seen += g.res[i].seen;
	log_msg("RESULT %d of %d steps got something, %u messages", seen, guide_nprompts, msgs);
	for (i = 0; i < g.cur; i++) {
		char sig[160], presses[96];
		int same = guide_same_as(&g, i);

		guide_signature(&g.res[i], sig, sizeof(sig));
		guide_press_list(&g.res[i], presses, sizeof(presses));
		log_msg("RESULT %-22s %s%s%s%s%s", guide_prompts[i].name, g.res[i].seen ? sig : "-",
		        same >= 0 ? "  SAME as " : "", same >= 0 ? guide_prompts[same].name : "",
		        presses[0] ? "  down at " : "", presses);
	}
	end = log_ms() + SUMMARY_MS;
	dirty = 1;
	while (guide_done(&g) && log_ms() < end && quit_poll() == QUIT_NONE) {
		if (screen_ok() && dirty) {
			draw_summary();
			dirty = 0;
		}
		nap(REDRAW_MS);
	}
}

static void run_free(unsigned seconds)
{
	unsigned end = log_ms() + seconds * 1000, left, shown_left = 0;
	int i;

	while (log_ms() < end && quit_poll() == QUIT_NONE) {
		left = (end - log_ms() + 999) / 1000;
		if (screen_ok() && (dirty || left != shown_left)) {
			draw_free(left);
			shown_left = left;
		}
		nap(REDRAW_MS);
	}
	log_msg("RESULT %u messages", msgs);
	for (i = 0; i < K_NCTL; i++)
		if (counts[i] != 0 || i < K_UNKNOWN)
			log_msg("RESULT %-17s %u%s", key_name(i), counts[i],
			        i <= K_VOL_DOWN ? " detents" : i == K_RELEASE ? "" : " presses");
}

int main(int argc, char *argv[])
{
	char logpath[256];
	unsigned seconds = 0, watchdog;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [guide | <seconds>]\n", argv[0]);
		return 2;
	}
	if (argc > 2 && atoi(argv[2]) > 0)
		seconds = (unsigned)atoi(argv[2]);
	watchdog = seconds ? seconds + WATCHDOG_SLACK
	                   : guide_total_ms() / 1000 + SUMMARY_MS / 1000 +
	                     WATCHDOG_SLACK;
	snprintf(logpath, sizeof(logpath), "%s/hello7.log", var_dir(argv[1]));
	if (log_open(logpath) != 0)
		fprintf(stderr, "hello7: cannot open %s, logging to stdout only\n", logpath);
	if (seconds)
		log_msg("hello7 start, pid=%d, free mode, %u s", getpid(), seconds);
	else
		log_msg("hello7 start, pid=%d, guided: %d steps, watchdog %u s", getpid(),
		        guide_nprompts, watchdog);

	/* quit.c owns /dev/ipc/ch8; on_key() runs on its reader thread. */
	quit_init_ex(argv[1], watchdog, on_key, NULL);
	cam_start();
	ui = screen_open() == 0;
	if (!ui)
		log_msg("FAIL hello7: no screen, logging only");
	else
		screen_set_reshow(RESHOW_MS);

	if (seconds)
		run_free(seconds);
	else
		run_guide();
	quit_request(QUIT_TIMER);

	if (cam_active())
		screen_no_show_on_close();
	screen_close();
	cam_stop();
	log_msg("hello7 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

/*
 * hello7 - knob and button tester: every /dev/ipc/ch8 message, decoded
 * (keys.c) and shown live, so any control can be identified by using it.
 *
 * Extracted from hello5's stage 2, which decoded the knobs and two buttons
 * against a fixed sequence on 2026-10-21 (docs/platform-notes.md section
 * 8). hello7 has no sequence: turn and press anything. The screen shows a
 * counter per known control (the last one used in yellow) and the latest
 * messages with their first 16 bytes. A bit no known control explains
 * shows as "unknown b<byte> 0x<bits>": press one button at a time to name
 * it.
 *
 * Note that the HMI still acts on every knob and button while hello7 is on
 * screen (docs/TODO.md).
 *
 * Output: <sd>/hello7.log, every message in full (27 bytes) with its
 * decoding and the time since the previous one, then RESULT counts.
 *
 * Stops after <seconds>, on SIGTERM/INT/HUP, SD removal or the watchdog
 * (src/common/quit.c). No key or button stops it.
 *
 * usage: hello7 <sd_path> [seconds]   (default 60)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "font_big.h"
#include "font_small.h"
#include "keys.h"
#include "log.h"
#include "quit.h"
#include "screen.h"
#include "text.h"

#define DEFAULT_SECONDS 60
#define WATCHDOG_SLACK  20
#define REDRAW_MS       100
#define RECENT          8       /* messages kept on screen */
#define MARGIN          20

#define C_BG     0xff101820u
#define C_TEXT   0xffe8e8e8u
#define C_DIM    0xff8a929eu
#define C_TITLE  0xffffd040u
#define C_HOT    0xffffb020u
#define C_BAR    0xff1c2836u

/* The counters on screen: the named controls, then unknown bits. */
static const int shown[] = {
	K_MAIN_RIGHT, K_MAIN_LEFT, K_VOL_UP, K_VOL_DOWN, K_MENU,
	K_SOFT_TL, K_B1_40, K_B1_80, K_UNKNOWN,
};
#define NSHOWN ((int)(sizeof(shown) / sizeof(shown[0])))

/* Written by on_key (quit.c's key thread), read by the drawing loop. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned counts[K_NCTL];         /* detents for knobs, presses else */
static unsigned msgs, last_ms;
static int last_ctl = -1;
static char recent[RECENT][112];
static unsigned nrecent;                /* lines ever added */
static int dirty = 1;

static void on_key(const unsigned char *buf, int n, void *arg)
{
	struct key_hit hits[KEY_MAX_HITS];
	char what[96], hex[32 * 3 + 1], gap_s[16];
	unsigned now = log_ms(), gap;
	int nh, i;

	(void)arg;
	nh = key_decode(buf, n, hits, KEY_MAX_HITS);
	key_describe(hits, nh, what, sizeof(what));
	for (i = 0; i < n && i < 32; i++)
		snprintf(hex + i * 3, 4, "%02x ", buf[i]);
	hex[i * 3] = '\0';

	pthread_mutex_lock(&lock);
	gap = msgs ? now - last_ms : 0;
	msgs++;
	last_ms = now;
	for (i = 0; i < nh; i++) {
		counts[hits[i].ctl] += (unsigned)hits[i].amount;
		if (hits[i].ctl != K_RELEASE)
			last_ctl = hits[i].ctl;
	}
	/* the screen shows the first 16 bytes, the log all of them */
	snprintf(gap_s, sizeof(gap_s), "+%u ms", gap);
	snprintf(recent[nrecent % RECENT], sizeof(recent[0]),
	         "#%-4u %-10s %-26.26s %.47s", msgs, gap_s, what, hex);
	nrecent++;
	dirty = 1;
	pthread_mutex_unlock(&lock);

	log_msg("key #%u (+%u ms): %s; %d bytes: %s", msgs, gap, what, n, hex);
}

static void draw(unsigned left_s)
{
	struct canvas *c = screen_canvas();
	char line[64];
	unsigned i, n, first;
	int x, y;

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	text_draw(c, &font_big, MARGIN, 8, "hello7  knobs and buttons", -1, C_TITLE);
	snprintf(line, sizeof(line), "t = %u ms", log_ms());
	text_draw(c, &font_big, c->w - MARGIN - (int)strlen(line) * font_big.w, 8,
	          line, -1, C_TEXT);
	text_draw(c, &font_small, MARGIN, 52,
	          "Turn any knob, press any button: each message shows up here.",
	          -1, C_DIM);

	pthread_mutex_lock(&lock);
	/* two columns of counters */
	for (i = 0; i < (unsigned)NSHOWN; i++) {
		x = MARGIN + (int)(i % 2) * 380;
		y = 80 + (int)(i / 2) * (font_big.h + 4);
		snprintf(line, sizeof(line), "%-17s %4u", key_name(shown[i]), counts[shown[i]]);
		text_draw(c, &font_big, x, y, line, -1,
		          shown[i] == last_ctl ? C_HOT : C_TEXT);
	}
	snprintf(line, sizeof(line), "%u messages", msgs);
	text_draw(c, &font_small, MARGIN, 262, line, -1, C_DIM);
	n = nrecent < RECENT ? nrecent : RECENT;
	first = nrecent - n;
	for (i = 0, y = 284; i < n; i++, y += font_small.h + 2)
		text_draw(c, &font_small, MARGIN, y, recent[(first + i) % RECENT], -1,
		          i + 1 == n ? C_TEXT : C_DIM);
	dirty = 0;
	pthread_mutex_unlock(&lock);

	canvas_fill(c, 0, c->h - 32, c->w, 32, C_BAR);
	snprintf(line, sizeof(line), "ends in %u s", left_s);
	text_draw(c, &font_small, MARGIN, c->h - 24, line, -1, C_TEXT);
	screen_present();
}

static void nap(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
	char logpath[256];
	unsigned seconds = DEFAULT_SECONDS, end, left, shown_left = 0;
	int ui, i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [seconds]\n", argv[0]);
		return 2;
	}
	if (argc > 2 && atoi(argv[2]) > 0)
		seconds = (unsigned)atoi(argv[2]);
	snprintf(logpath, sizeof(logpath), "%s/hello7.log", argv[1]);
	if (log_open(logpath) != 0)
		fprintf(stderr, "hello7: cannot open %s, logging to stdout only\n", logpath);
	log_msg("hello7 start, pid=%d, %u s", getpid(), seconds);

	/* quit.c owns /dev/ipc/ch8; on_key() runs on its reader thread. */
	quit_init_ex(argv[1], seconds + WATCHDOG_SLACK, on_key, NULL);
	ui = screen_open() == 0;
	if (!ui)
		log_msg("FAIL hello7: no screen, logging only");

	end = log_ms() + seconds * 1000;
	while (log_ms() < end && quit_poll() == QUIT_NONE) {
		left = (end - log_ms() + 999) / 1000;
		if (ui && (dirty || left != shown_left)) {
			draw(left);
			shown_left = left;
		}
		nap(REDRAW_MS);
	}
	quit_request(QUIT_TIMER);

	log_msg("RESULT %u messages", msgs);
	for (i = 0; i < K_NCTL; i++)
		if (counts[i] != 0 || i < K_UNKNOWN)
			log_msg("RESULT %-17s %u%s", key_name(i), counts[i],
			        i <= K_VOL_DOWN ? " detents" : i == K_RELEASE ? "" : " presses");
	screen_close();
	log_msg("hello7 exit (stop: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

/*
 * guide.h - hello7's guided run: one control at a time. The screen says
 * what to do; every ch8 message that arrives meanwhile is put down for
 * that prompt. A prompt ends quiet_ms after the last message it wants, or
 * after idle_ms with nothing (the car has no such control), or after
 * guide_max_ms() whatever happens.
 *
 * Only what is still open is asked: the steering wheel's volume roller and
 * the touch pad (in the current screen, in every mode hello7 can set, and
 * on the HMI's own radio, map and speller screens). The panel's knobs and
 * buttons are all named (src/common/buttons/README.md).
 *
 * What a prompt got is its signature: steering wheel codes with their
 * ticks ("wheel 12 +3"), touch pad subfunctions ("pad ca cb 3d 3e"), the
 * pad mode it echoed ("mode 6") and any panel control bytes ("b1 0x02").
 * Two prompts with the same signature (the mode aside) got the same thing.
 *
 * Pure (no car APIs), tested on the host (tests/test_guide.c).
 */
#ifndef HELLO7_GUIDE_H
#define HELLO7_GUIDE_H

#include "types.h"

#define GUIDE_BYTES    13       /* panel control bytes: 0-12 */
#define GUIDE_MAX      40
#define GUIDE_QUIET_MS 2000
#define GUIDE_IDLE_MS  15000
#define GUIDE_MAX_MS   30000    /* at least; longer idle times get 15 s more */
#define GUIDE_PRESSES  6        /* finger-down positions kept */
#define GUIDE_WHEEL    4        /* wheel codes kept */

enum guide_want {
	WANT_ANY,               /* every control counts */
	WANT_PAD,               /* only what a finger on the pad makes */
	WANT_WHEEL,             /* only steering wheel keys */
};

struct guide_prompt {
	const char *name;       /* short, for the log and the summary */
	const char *text;       /* what to do, big on screen */
	const char *hint;       /* small, under it */
	int want;               /* enum guide_want */
	int set_mode;           /* > 0: hello7 sets this pad mode first */
	int hmi;                /* the HMI's screens come up: hello7 doesn't re-show */
	unsigned idle_ms;       /* 0: GUIDE_IDLE_MS */
	unsigned quiet_ms;      /* 0: GUIDE_QUIET_MS */
};

extern const struct guide_prompt guide_prompts[];
extern const int guide_nprompts;

struct guide_result {
	unsigned msgs;          /* messages, releases included */
	unsigned presses;       /* messages that count (the prompt's want) */
	unsigned begin_ms, end_ms, first_ms, last_ms;
	int knob[GUIDE_BYTES];  /* detents, knob bytes only */
	int bits[GUIDE_BYTES];  /* OR of every message, other bytes */
	u32 subs[8];            /* touch pad subfunctions seen (bitmap) */
	int mode;               /* the last pad mode echoed, -1 none */
	int nwheel;
	int wheel_code[GUIDE_WHEEL];
	int wheel_sum[GUIDE_WHEEL];     /* roller ticks, key messages */
	int npress;
	u8 press_x[GUIDE_PRESSES], press_y[GUIDE_PRESSES];
	int seen;               /* something the prompt wants came */
	int idle;               /* ended with nothing seen */
	int capped;             /* ended by guide_max_ms() */
};

struct guide {
	int cur;                /* the prompt now; guide_nprompts when done */
	struct guide_result res[GUIDE_MAX];
};

unsigned guide_idle_ms(const struct guide_prompt *p);
unsigned guide_quiet_ms(const struct guide_prompt *p);
unsigned guide_max_ms(const struct guide_prompt *p);
/* Every prompt at its longest. */
unsigned guide_total_ms(void);

void guide_start(struct guide *g, unsigned now);
/* A ch8 message during the current prompt. */
void guide_key(struct guide *g, const u8 *buf, int n, unsigned now);
/* 1 if the current prompt ended now (g->cur moved on to the next). */
int guide_tick(struct guide *g, unsigned now);
int guide_done(const struct guide *g);

/* Milliseconds until the current prompt ends by itself, if nothing new
 * comes: the quiet time once something was seen, else the idle time. */
unsigned guide_left_ms(const struct guide *g, unsigned now);

/* "wheel 12 +3, pad ca cb, mode 6, b1 0x02", "nothing". */
void guide_signature(const struct guide_result *r, char *buf, int size);
/* "132,198 20,30", "" if no finger came down. */
void guide_press_list(const struct guide_result *r, char *buf, int size);
/* An earlier prompt with the same (non-empty) signature, or -1. */
int guide_same_as(const struct guide *g, int i);

#endif

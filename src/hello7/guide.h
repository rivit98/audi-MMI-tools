/*
 * guide.h - hello7's guided run: one control at a time. The screen says
 * what to press or turn; every ch8 message that arrives meanwhile is put
 * down for that prompt. A prompt ends GUIDE_QUIET_MS after the last
 * message, or after GUIDE_IDLE_MS with nothing (the car has no such
 * control), or after GUIDE_MAX_MS whatever happens.
 *
 * What a prompt got is its signature: the detents per knob byte (4-7) and
 * the bits seen in every other control byte (keys.c's key_bits), e.g.
 * "b6 +3" or "b1 0x02". Two prompts with the same signature are the same
 * control, or one of them was missed.
 *
 * Pure (no car APIs), tested on the host (tests/test_guide.c).
 */
#ifndef HELLO7_GUIDE_H
#define HELLO7_GUIDE_H

#define GUIDE_BYTES    16       /* control bytes: 0-15 (16-26 are unit state) */
#define GUIDE_MAX      40
#define GUIDE_QUIET_MS 2000
#define GUIDE_IDLE_MS  15000
#define GUIDE_MAX_MS   30000

struct guide_prompt {
	const char *name;       /* short, for the log and the summary */
	const char *text;       /* what to do, big on screen */
	const char *hint;       /* small, under it */
};

extern const struct guide_prompt guide_prompts[];
extern const int guide_nprompts;

struct guide_result {
	unsigned msgs;          /* messages, releases included */
	unsigned presses;       /* messages with a control in them */
	unsigned begin_ms, end_ms, first_ms, last_ms;
	int knob[GUIDE_BYTES];  /* detents, knob bytes only */
	int bits[GUIDE_BYTES];  /* OR of every message, other bytes */
	int seen;               /* a control was in a message */
	int idle;               /* ended with nothing seen */
	int capped;             /* ended by GUIDE_MAX_MS */
};

struct guide {
	int cur;                /* the prompt now; guide_nprompts when done */
	struct guide_result res[GUIDE_MAX];
};

void guide_start(struct guide *g, unsigned now);
/* A ch8 message during the current prompt. */
void guide_key(struct guide *g, const unsigned char *buf, int n, unsigned now);
/* 1 if the current prompt ended now (g->cur moved on to the next). */
int guide_tick(struct guide *g, unsigned now);
int guide_done(const struct guide *g);

/* Milliseconds until the current prompt ends by itself, if nothing new
 * comes: the quiet time once something was seen, else the idle time. */
unsigned guide_left_ms(const struct guide *g, unsigned now);

/* "b6 +3, b1 0x02", "nothing". */
void guide_signature(const struct guide_result *r, char *buf, int size);
/* An earlier prompt with the same (non-empty) signature, or -1. */
int guide_same_as(const struct guide *g, int i);

#endif

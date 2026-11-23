/*
 * guide.c - see guide.h.
 */
#include <stdio.h>
#include <string.h>

#include "guide.h"
#include "keys.h"
#include "tpad.h"
#include "types.h"

#define NOTHING   "Nothing seen is an answer too."
#define LIFT      "Lift your finger in between; it moves on 3 s after you stop."
#define PAD_QUIET 3000
#define HMI_IDLE  40000
#define HMI_QUIET 4000
#define HMI_BACK  "hello7 comes back by itself 4 s after you stop."

/* The modes hello7 sets: every one MMI3GApplication passes on (tpad.c). */
#define MODE(m) \
	{ "pad mode " #m, "Touch pad mode " #m " (set by hello7): draw a line LEFT to " \
	  "RIGHT, then write the letter A, then tap once.", \
	  "Pause no longer than 3 s. The pad answers with \"mode " #m "\" first.", \
	  WANT_PAD, m, 0, 0, PAD_QUIET }

const struct guide_prompt guide_prompts[] = {
	/* the steering wheel: on ch8 as bytes 23-25 (tpad.h) */
	{ "wheel volume up x3", "Steering wheel: roll the VOLUME wheel UP (louder) 3 clicks.",
	  "Ignition on. " NOTHING, WANT_WHEEL, 0, 0, 0, 0 },
	{ "wheel volume down x3", "Steering wheel: roll the VOLUME wheel DOWN 3 clicks.",
	  NOTHING, WANT_WHEEL, 0, 0, 0, 0 },

	/* the touch pad, in whatever mode the HMI left it */
	{ "pad tap", "Touch pad: TAP it once in the middle.", NOTHING,
	  WANT_PAD, 0, 0, 0, PAD_QUIET },
	{ "pad double tap", "Touch pad: DOUBLE TAP it in the middle.", NOTHING,
	  WANT_PAD, 0, 0, 0, PAD_QUIET },
	{ "pad corners", "Touch pad: touch the 4 CORNERS in turn: top left, top right, "
	  "bottom right, bottom left.", LIFT, WANT_PAD, 0, 0, 0, PAD_QUIET },
	{ "pad line right", "Touch pad: draw one slow line from LEFT to RIGHT across the middle.",
	  NOTHING, WANT_PAD, 0, 0, 0, PAD_QUIET },
	{ "pad line down", "Touch pad: draw one slow line from TOP to BOTTOM across the middle.",
	  NOTHING, WANT_PAD, 0, 0, 0, PAD_QUIET },

	/* every recognizer mode, set by hello7 (22 .. 02 00 C4 <mode> on ch8) */
	MODE(6), MODE(9), MODE(10), MODE(11), MODE(12), MODE(13), MODE(16), MODE(17),
	MODE(18),

	/* the HMI's own screens set their mode: which one, and what it gives */
	{ "radio screen", "Press RADIO. On the radio screen, slide one finger slowly over "
	  "the whole pad.", HMI_BACK, WANT_PAD, 0, 1, HMI_IDLE, HMI_QUIET },
	{ "map screen", "Press NAV (or MAP). On the map, drag one finger across the pad, "
	  "then tap it.", HMI_BACK, WANT_PAD, 0, 1, HMI_IDLE, HMI_QUIET },
	{ "speller", "Open a speller (NAV, enter a destination, the town field). Then "
	  "write the letter A on the pad with one finger.",
	  "Take your time: it waits 60 s. " HMI_BACK, WANT_PAD, 0, 1, 60000, HMI_QUIET },
};
const int guide_nprompts = (int)(sizeof(guide_prompts) / sizeof(guide_prompts[0]));

unsigned guide_idle_ms(const struct guide_prompt *p)
{
	return p->idle_ms ? p->idle_ms : GUIDE_IDLE_MS;
}

unsigned guide_quiet_ms(const struct guide_prompt *p)
{
	return p->quiet_ms ? p->quiet_ms : GUIDE_QUIET_MS;
}

unsigned guide_max_ms(const struct guide_prompt *p)
{
	unsigned m = guide_idle_ms(p) + 15000;

	return m > GUIDE_MAX_MS ? m : GUIDE_MAX_MS;
}

unsigned guide_total_ms(void)
{
	unsigned t = 0;
	int i;

	for (i = 0; i < guide_nprompts; i++)
		t += guide_max_ms(&guide_prompts[i]);
	return t;
}

static int is_knob(int i)
{
	return i >= 4 && i <= 7;
}

void guide_start(struct guide *g, unsigned now)
{
	int i;

	memset(g, 0, sizeof(*g));
	for (i = 0; i < GUIDE_MAX; i++)
		g->res[i].mode = -1;
	g->res[0].begin_ms = now;
}

static void add_wheel(struct guide_result *r, const struct mfl_hit *h)
{
	int i;

	for (i = 0; i < r->nwheel; i++)
		if (r->wheel_code[i] == h->code)
			break;
	if (i == r->nwheel) {
		if (r->nwheel >= GUIDE_WHEEL)
			return;
		r->wheel_code[r->nwheel++] = h->code;
	}
	r->wheel_sum[i] += h->roller ? h->ticks : 1;
}

void guide_key(struct guide *g, const u8 *buf, int n, unsigned now)
{
	const struct guide_prompt *p;
	struct guide_result *r;
	struct mfl_hit wh[MFL_MAX];
	int i, nw, sub, panel = 0, pad = 0, counts = 0;

	if (guide_done(g))
		return;
	p = &guide_prompts[g->cur];
	r = &g->res[g->cur];
	r->msgs++;
	for (i = 0; i < GUIDE_BYTES; i++) {
		int b = key_bits(buf, n, i);

		if (b == 0)
			continue;
		panel = 1;
		if (is_knob(i))
			r->knob[i] += b;
		else
			r->bits[i] |= b;
	}
	nw = mfl_decode(buf, n, wh, MFL_MAX);
	for (i = 0; i < nw; i++)
		add_wheel(r, &wh[i]);
	sub = tpad_sub(buf, n);
	if (sub >= 0) {
		r->subs[sub >> 5] |= 1u << (sub & 31);
		pad = tpad_is_user(sub);
		if (sub == 0xc4)
			r->mode = tpad_mode_echo(buf, n);
		if (sub == 0xca && n >= 30 && r->npress < GUIDE_PRESSES) {
			r->press_x[r->npress] = buf[28];
			r->press_y[r->npress] = buf[29];
			r->npress++;
		}
	}
	switch (p->want) {
	case WANT_PAD:   counts = pad; break;
	case WANT_WHEEL: counts = nw > 0; break;
	default:         counts = panel || pad || nw > 0; break;
	}
	if (!counts)
		return;                 /* a release, or not what the prompt asks */
	if (!r->seen)
		r->first_ms = now;
	r->seen = 1;
	r->presses++;
	r->last_ms = now;
}

static void next(struct guide *g, unsigned now)
{
	g->res[g->cur].end_ms = now;
	g->cur++;
	if (!guide_done(g))
		g->res[g->cur].begin_ms = now;
}

int guide_tick(struct guide *g, unsigned now)
{
	const struct guide_prompt *p;
	struct guide_result *r;

	if (guide_done(g))
		return 0;
	p = &guide_prompts[g->cur];
	r = &g->res[g->cur];
	if (now - r->begin_ms >= guide_max_ms(p))
		r->capped = 1;
	else if (r->seen && now - r->last_ms >= guide_quiet_ms(p))
		;
	else if (!r->seen && now - r->begin_ms >= guide_idle_ms(p))
		r->idle = 1;
	else
		return 0;
	next(g, now);
	return 1;
}

int guide_done(const struct guide *g)
{
	return g->cur >= guide_nprompts || g->cur >= GUIDE_MAX;
}

unsigned guide_left_ms(const struct guide *g, unsigned now)
{
	const struct guide_prompt *p;
	const struct guide_result *r;
	unsigned end;

	if (guide_done(g))
		return 0;
	p = &guide_prompts[g->cur];
	r = &g->res[g->cur];
	end = r->seen ? r->last_ms + guide_quiet_ms(p) : r->begin_ms + guide_idle_ms(p);
	if (end > r->begin_ms + guide_max_ms(p))
		end = r->begin_ms + guide_max_ms(p);
	return end > now ? end - now : 0;
}

#define ADD(...) \
	do { \
		if (len < size) \
			len += snprintf(buf + len, size - len, __VA_ARGS__); \
	} while (0)

void guide_signature(const struct guide_result *r, char *buf, int size)
{
	int i, len = 0, any = 0;

	buf[0] = '\0';
	for (i = 0; i < r->nwheel; i++)
		ADD("%swheel %02x %+d", len ? ", " : "", r->wheel_code[i], r->wheel_sum[i]);
	for (i = 0; i < 256; i++) {
		if (!(r->subs[i >> 5] & (1u << (i & 31))))
			continue;
		ADD("%s%02x", any ? " " : len ? ", pad " : "pad ", i);
		any = 1;
	}
	if (r->mode >= 0)
		ADD("%smode %d", len ? ", " : "", r->mode);
	for (i = 0; i < GUIDE_BYTES; i++) {
		if (r->knob[i] != 0)
			ADD("%sb%d +%d", len ? ", " : "", i, r->knob[i]);
		else if (r->bits[i] != 0)
			ADD("%sb%d 0x%02x", len ? ", " : "", i, r->bits[i]);
	}
	if (len == 0)
		snprintf(buf, size, "nothing");
}

void guide_press_list(const struct guide_result *r, char *buf, int size)
{
	int i, len = 0;

	buf[0] = '\0';
	for (i = 0; i < r->npress; i++)
		ADD("%s%d,%d", i ? " " : "", r->press_x[i], r->press_y[i]);
}

/* Same bytes, bits, wheel codes and pad subfunctions; knobs compare by
 * which bytes turned, not how far; rollers by code and direction (up and
 * down are different answers); the echoed mode doesn't count. */
static int same(const struct guide_result *a, const struct guide_result *b)
{
	int i;

	for (i = 0; i < GUIDE_BYTES; i++)
		if ((a->knob[i] != 0) != (b->knob[i] != 0) || a->bits[i] != b->bits[i])
			return 0;
	for (i = 0; i < 8; i++)
		if (a->subs[i] != b->subs[i])
			return 0;
	if (a->nwheel != b->nwheel)
		return 0;
	for (i = 0; i < a->nwheel; i++)
		if (a->wheel_code[i] != b->wheel_code[i] ||
		    (a->wheel_sum[i] > 0) != (b->wheel_sum[i] > 0) ||
		    (a->wheel_sum[i] < 0) != (b->wheel_sum[i] < 0))
			return 0;
	return 1;
}

int guide_same_as(const struct guide *g, int i)
{
	int j;

	if (i < 0 || i >= g->cur || !g->res[i].seen)
		return -1;
	for (j = 0; j < i; j++)
		if (g->res[j].seen && same(&g->res[j], &g->res[i]))
			return j;
	return -1;
}

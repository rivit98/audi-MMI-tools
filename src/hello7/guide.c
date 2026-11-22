/*
 * guide.c - see guide.h.
 */
#include <stdio.h>
#include "types.h"
#include <string.h>

#include "guide.h"
#include "keys.h"

#define NO_SUCH "No such control? Touch nothing; it moves on in 15 s."

const struct guide_prompt guide_prompts[] = {
	{ "main knob right x3", "Turn the MAIN knob 3 clicks RIGHT (clockwise).",
	  "The big knob in the middle of the control panel." },
	{ "main knob left x3", "Turn the MAIN knob 3 clicks LEFT.", "" },
	{ "main knob press", "PRESS the main knob once (push it down).", "" },
	{ "joystick up", "Tilt the main knob UP (forward), once.",
	  "Only if the knob tilts like a joystick. " NO_SUCH },
	{ "joystick down", "Tilt the main knob DOWN (towards you), once.", NO_SUCH },
	{ "joystick left", "Tilt the main knob LEFT, once.", NO_SUCH },
	{ "joystick right", "Tilt the main knob RIGHT, once.", NO_SUCH },
	{ "soft key top left", "Press the soft key at the TOP LEFT of the main knob.",
	  "The four small keys around the main knob." },
	{ "soft key top right", "Press the soft key at the TOP RIGHT of the main knob.", "" },
	{ "soft key bottom left", "Press the soft key at the BOTTOM LEFT of the main knob.", "" },
	{ "soft key bottom right", "Press the soft key at the BOTTOM RIGHT of the main knob.", "" },
	{ "BACK", "Press BACK (the return key).", "" },
	{ "MENU", "Press MENU.", "The MMI opens its menu; this screen comes back by itself." },
	{ "NAV/MAP", "Press NAV (or MAP).", NO_SUCH },
	{ "TEL", "Press TEL.", NO_SUCH },
	{ "RADIO", "Press RADIO.", NO_SUCH },
	{ "MEDIA", "Press MEDIA.", NO_SUCH },
	{ "CAR", "Press CAR.", NO_SUCH },
	{ "volume up x3", "Turn the VOLUME knob 3 clicks UP (louder).",
	  "The small knob. Turn it back in the next step." },
	{ "volume down x3", "Turn the VOLUME knob 3 clicks DOWN.", "" },
	{ "volume press (mute)", "PRESS the volume knob once (mute).",
	  "Press it again in the next step to unmute." },
	{ "volume press (unmute)", "PRESS the volume knob once more (unmute).", "" },
	{ "skip previous", "Press the PREVIOUS track key (|<<).",
	  "Next to the volume knob. " NO_SUCH },
	{ "skip next", "Press the NEXT track key (>>|).", NO_SUCH },
	{ "touch pad", "Touch pad: draw a short line with one finger.", NO_SUCH },
	{ "wheel volume up", "Steering wheel: roll the VOLUME wheel up 3 clicks.",
	  "The wheel's keys may not come this way: nothing seen is an answer too." },
	{ "wheel left/right", "Steering wheel: press its NEXT (or right arrow) key.",
	  "Nothing seen is an answer too." },
};
const int guide_nprompts = (int)(sizeof(guide_prompts) / sizeof(guide_prompts[0]));

static int is_knob(int i)
{
	return i >= 4 && i <= 7;
}

void guide_start(struct guide *g, unsigned now)
{
	memset(g, 0, sizeof(*g));
	g->res[0].begin_ms = now;
}

void guide_key(struct guide *g, const u8 *buf, int n, unsigned now)
{
	struct guide_result *r;
	int i, any = 0;

	if (guide_done(g))
		return;
	r = &g->res[g->cur];
	r->msgs++;
	for (i = 0; i < GUIDE_BYTES; i++) {
		int b = key_bits(buf, n, i);

		if (b == 0)
			continue;
		any = 1;
		if (is_knob(i))
			r->knob[i] += b;
		else
			r->bits[i] |= b;
	}
	if (!any)
		return;                 /* a release */
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
	struct guide_result *r;

	if (guide_done(g))
		return 0;
	r = &g->res[g->cur];
	if (now - r->begin_ms >= GUIDE_MAX_MS)
		r->capped = 1;
	else if (r->seen && now - r->last_ms >= GUIDE_QUIET_MS)
		;
	else if (!r->seen && now - r->begin_ms >= GUIDE_IDLE_MS)
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
	const struct guide_result *r;
	unsigned end;

	if (guide_done(g))
		return 0;
	r = &g->res[g->cur];
	end = r->seen ? r->last_ms + GUIDE_QUIET_MS : r->begin_ms + GUIDE_IDLE_MS;
	if (end > r->begin_ms + GUIDE_MAX_MS)
		end = r->begin_ms + GUIDE_MAX_MS;
	return end > now ? end - now : 0;
}

void guide_signature(const struct guide_result *r, char *buf, int size)
{
	int i, len = 0;

	buf[0] = '\0';
	for (i = 0; i < GUIDE_BYTES && len < size; i++) {
		const char *sep = len ? ", " : "";

		if (r->knob[i] != 0)
			len += snprintf(buf + len, size - len, "%sb%d +%d", sep, i, r->knob[i]);
		else if (r->bits[i] != 0)
			len += snprintf(buf + len, size - len, "%sb%d 0x%02x", sep, i, r->bits[i]);
	}
	if (len == 0)
		snprintf(buf, size, "nothing");
}

/* Same bytes and bits; knobs compare by which bytes turned, not how far. */
static int same(const struct guide_result *a, const struct guide_result *b)
{
	int i;

	for (i = 0; i < GUIDE_BYTES; i++)
		if ((a->knob[i] != 0) != (b->knob[i] != 0) || a->bits[i] != b->bits[i])
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

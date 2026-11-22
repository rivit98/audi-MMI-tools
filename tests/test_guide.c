/*
 * test_guide.c - hello7's guided run (src/hello7/guide.c): when a prompt
 * ends, what it records, signatures and duplicates. Messages as logged on
 * the car (src/common/buttons/README.md).
 */
#include "check.h"
#include "types.h"
#include "guide.h"
#include "keys.h"
#include "tpad.h"

/* a 27-byte panel message: one byte changed */
static void msg(u8 *m, int byte, int value)
{
	memset(m, 0, 40);
	m[14] = 0x01;
	m[15] = 0x02;
	m[17] = 0x01;
	if (byte >= 0)
		m[byte] = (u8)value;
}

/* a steering wheel message: source 3, code, event nibble, brand/variant */
static void wheel(u8 *m, int code, int ev)
{
	msg(m, -1, 0);
	m[16] = 0x03;
	m[23] = (u8)code;
	m[25] = (u8)ev;
	m[26] = 0x11;
}

/* a touch pad message: kind 02, subfunction + data from byte 27; returns n */
static int pad(u8 *m, int sub, int a, int b)
{
	int n = 28;

	msg(m, -1, 0);
	m[14] = 0x02;
	m[27] = (u8)sub;
	if (a >= 0)
		m[n++] = (u8)a;
	if (b >= 0)
		m[n++] = (u8)b;
	return n;
}

static int find(const char *name)
{
	for (int i = 0; i < guide_nprompts; i++)
		if (strcmp(guide_prompts[i].name, name) == 0)
			return i;
	return -1;
}

/* run the guide to prompt i, every earlier one idling out */
static void skip_to(struct guide *g, int i)
{
	while (g->cur < i)
		guide_tick(g, g->res[g->cur].begin_ms + guide_idle_ms(&guide_prompts[g->cur]));
}

int main(void)
{
	static struct guide g;
	u8 m[40];
	char sig[160];
	int n, i;

	CHECK(guide_nprompts > 10 && guide_nprompts <= GUIDE_MAX);
	CHECK(guide_total_ms() >= (unsigned)guide_nprompts * GUIDE_MAX_MS);

	/* every recognizer mode MMI3GApplication passes on has a prompt */
	for (i = 0; i < tpad_nmodes; i++) {
		int found = 0;

		for (int j = 0; j < guide_nprompts; j++)
			found |= guide_prompts[j].set_mode == tpad_modes[i];
		CHECK(found);
	}

	guide_start(&g, 1000);
	CHECK_EQ(g.cur, 0);
	CHECK_EQ(g.res[0].mode, -1);
	CHECK_EQ(guide_left_ms(&g, 1000), GUIDE_IDLE_MS);

	/* prompt 0, wheel volume up: a main knob turn doesn't count; three
	 * roller ticks with releases do */
	CHECK_EQ(guide_prompts[0].want, WANT_WHEEL);
	msg(m, 6, 1);
	guide_key(&g, m, 27, 1100);
	CHECK(!g.res[0].seen);
	wheel(m, 0x12, 0x01);
	guide_key(&g, m, 27, 1500);
	wheel(m, 0, 0);
	guide_key(&g, m, 27, 1550);
	wheel(m, 0x12, 0x01);
	guide_key(&g, m, 27, 1700);
	wheel(m, 0x12, 0x01);
	guide_key(&g, m, 27, 1800);
	CHECK_EQ(guide_tick(&g, 1800 + GUIDE_QUIET_MS - 1), 0);
	CHECK_EQ(guide_tick(&g, 1800 + GUIDE_QUIET_MS), 1);
	CHECK_EQ(g.res[0].presses, 3);
	guide_signature(&g.res[0], sig, sizeof(sig));
	CHECK_STR(sig, "wheel 12 +3, b6 +1");

	/* prompt 1, wheel volume down: -1 ticks */
	unsigned t = g.res[1].begin_ms;
	wheel(m, 0x12, 0x0f);
	guide_key(&g, m, 27, t + 10);
	wheel(m, 0x12, 0x0f);
	guide_key(&g, m, 27, t + 20);
	guide_tick(&g, t + 20 + GUIDE_QUIET_MS);
	guide_signature(&g.res[1], sig, sizeof(sig));
	CHECK_STR(sig, "wheel 12 -2");
	CHECK_EQ(guide_same_as(&g, 1), -1);   /* same roller, but 0 also got a knob turn */

	/* the pad prompts: a recMode echo after a button doesn't count, a
	 * stroke does; the finger-down position is kept */
	i = find("pad corners");
	CHECK(i > 1);
	skip_to(&g, i);
	CHECK_EQ(guide_prompts[i].want, WANT_PAD);
	t = g.res[i].begin_ms;
	n = pad(m, 0xc4, 0x06, -1);
	guide_key(&g, m, n, t + 100);
	CHECK(!g.res[i].seen);
	CHECK_EQ(g.res[i].mode, 6);
	n = pad(m, 0xca, 0x84, 0xc6);
	CHECK_EQ(n, 30);
	guide_key(&g, m, n, t + 200);
	n = pad(m, 0x3d, 0x3c, -1);
	guide_key(&g, m, n, t + 300);
	n = pad(m, 0x3e, 0x3c, -1);
	guide_key(&g, m, n, t + 400);
	n = pad(m, 0xcb, -1, -1);
	guide_key(&g, m, n, t + 500);
	n = pad(m, 0xca, 0x10, 0x20);
	guide_key(&g, m, n, t + 2600);       /* within the pad's 3 s */
	CHECK_EQ(guide_tick(&g, t + 2600 + 2999), 0);
	CHECK_EQ(guide_quiet_ms(&guide_prompts[i]), 3000u);
	CHECK_EQ(guide_tick(&g, t + 2600 + 3000), 1);
	guide_signature(&g.res[i], sig, sizeof(sig));
	CHECK_STR(sig, "pad 3d 3e c4 ca cb, mode 6");
	guide_press_list(&g.res[i], sig, sizeof(sig));
	CHECK_STR(sig, "132,198 16,32");

	/* a mode prompt sets its mode; the next one with the same strokes is
	 * the SAME although its mode differs */
	i = find("pad mode 6");
	CHECK(i > 0);
	CHECK_EQ(guide_prompts[i].set_mode, 6);
	CHECK_EQ(guide_prompts[i + 1].set_mode, 9);
	skip_to(&g, i);
	for (int k = 0; k < 2; k++) {
		t = g.res[i + k].begin_ms;
		n = pad(m, 0xc4, k ? 9 : 6, -1);
		guide_key(&g, m, n, t + 50);
		n = pad(m, 0xd3, 0x00, -1);
		guide_key(&g, m, n, t + 100);
		guide_tick(&g, t + 100 + 3000);
	}
	guide_signature(&g.res[i + 1], sig, sizeof(sig));
	CHECK_STR(sig, "pad c4 d3, mode 9");
	CHECK_EQ(guide_same_as(&g, i + 1), i);

	/* the HMI screens wait longer, and don't count panel buttons */
	i = find("speller");
	CHECK(i > 0);
	CHECK(guide_prompts[i].hmi);
	CHECK_EQ(guide_idle_ms(&guide_prompts[i]), 60000u);
	CHECK_EQ(guide_max_ms(&guide_prompts[i]), 75000u);
	skip_to(&g, i);
	t = g.res[i].begin_ms;
	msg(m, 0, 0x10);                     /* NAV */
	guide_key(&g, m, 27, t + 100);
	CHECK(!g.res[i].seen);
	CHECK_EQ(guide_tick(&g, t + 59999), 0);
	m[0] = 0;
	n = pad(m, 0xc7, 0x00, 0x01);
	m[30] = 'A';
	guide_key(&g, m, 31, t + 59000);
	CHECK(g.res[i].seen);
	guide_signature(&g.res[i], sig, sizeof(sig));
	CHECK_STR(sig, "pad c7, b0 0x10");

	/* capped by the prompt's own maximum */
	for (unsigned k = 59000; k <= 80000; k += 1000) {
		guide_key(&g, m, 31, t + k);
		if (guide_tick(&g, t + k))
			break;
	}
	CHECK(g.res[i].capped);
	CHECK_EQ(g.res[i].end_ms, t + 75000);

	/* to the end: nothing more happens */
	skip_to(&g, guide_nprompts);
	CHECK(guide_done(&g));
	CHECK_EQ(guide_tick(&g, 9999999), 0);
	guide_key(&g, m, 27, 9999999);
	CHECK_EQ(guide_left_ms(&g, 9999999), 0);

	/* key_bits: panel bytes 0-12 only */
	msg(m, 2, 0x80);
	CHECK_EQ(key_bits(m, 27, 2), 0x80);
	CHECK_EQ(key_bits(m, 27, 14), 0);
	CHECK_EQ(key_bits(m, 27, 15), 0);
	CHECK_EQ(key_bits(m, 27, 16), 0);
	CHECK_EQ(key_bits(m, 10, 12), 0);
	CHECK_DONE();
}

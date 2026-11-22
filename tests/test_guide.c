/*
 * test_guide.c - hello7's guided run (src/hello7/guide.c): when a prompt
 * ends, what it records, signatures and duplicates.
 */
#include "check.h"
#include "guide.h"
#include "keys.h"

/* a 27-byte ch8 message: constant bytes set, one byte changed, unit state noise */
static void msg(unsigned char *m, int byte, int value)
{
	memset(m, 0, 27);
	m[14] = 0x01;
	m[15] = 0x02;
	m[16] = 0x03;           /* unit state: never a control */
	m[17] = 0x01;
	if (byte >= 0)
		m[byte] = (unsigned char)value;
}

int main(void)
{
	static struct guide g;
	unsigned char m[27];
	char sig[128];

	CHECK(guide_nprompts > 20 && guide_nprompts <= GUIDE_MAX);
	guide_start(&g, 1000);
	CHECK_EQ(g.cur, 0);
	CHECK_EQ(guide_left_ms(&g, 1000), GUIDE_IDLE_MS);

	/* prompt 0: main knob 1 + 2 detents, no release after a knob */
	msg(m, 6, 1);
	guide_key(&g, m, 27, 1500);
	msg(m, 6, 2);
	guide_key(&g, m, 27, 1800);
	CHECK_EQ(guide_tick(&g, 1800 + GUIDE_QUIET_MS - 1), 0);
	CHECK_EQ(guide_left_ms(&g, 1800), GUIDE_QUIET_MS);
	CHECK_EQ(guide_tick(&g, 1800 + GUIDE_QUIET_MS), 1);
	CHECK_EQ(g.cur, 1);
	CHECK_EQ(g.res[0].presses, 2);
	CHECK_EQ(g.res[0].knob[6], 3);
	guide_signature(&g.res[0], sig, sizeof(sig));
	CHECK_STR(sig, "b6 +3");

	/* prompt 1: a release alone doesn't count; then nothing: idle */
	msg(m, -1, 0);
	guide_key(&g, m, 27, 4000);
	CHECK_EQ(g.res[1].msgs, 1);
	CHECK(!g.res[1].seen);
	CHECK_EQ(guide_tick(&g, 3800 + GUIDE_IDLE_MS), 1);
	CHECK(g.res[1].idle);
	guide_signature(&g.res[1], sig, sizeof(sig));
	CHECK_STR(sig, "nothing");
	CHECK_EQ(guide_same_as(&g, 1), -1);

	/* prompt 2: a button, press + release; the quiet time counts from the
	 * press (a release is not a control) */
	unsigned t = g.res[2].begin_ms;
	msg(m, 1, 0x02);
	guide_key(&g, m, 27, t + 100);
	msg(m, -1, 0);
	guide_key(&g, m, 27, t + 300);
	CHECK_EQ(guide_tick(&g, t + 100 + GUIDE_QUIET_MS), 1);
	guide_signature(&g.res[2], sig, sizeof(sig));
	CHECK_STR(sig, "b1 0x02");

	/* prompt 3: the same button again: a duplicate of 2; prompt 4: knob
	 * byte 6 once: same control as 0 (detents don't matter) */
	t = g.res[3].begin_ms;
	msg(m, 1, 0x02);
	guide_key(&g, m, 27, t + 10);
	guide_tick(&g, t + 10 + GUIDE_QUIET_MS);
	t = g.res[4].begin_ms;
	msg(m, 6, 1);
	guide_key(&g, m, 27, t + 10);
	guide_tick(&g, t + 10 + GUIDE_QUIET_MS);
	CHECK_EQ(guide_same_as(&g, 3), 2);
	CHECK_EQ(guide_same_as(&g, 4), 0);
	CHECK_EQ(guide_same_as(&g, 5), -1);   /* not done yet */

	/* prompt 5: someone keeps turning: capped at GUIDE_MAX_MS */
	t = g.res[5].begin_ms;
	for (unsigned k = 0; k <= GUIDE_MAX_MS; k += 500) {
		msg(m, 4, 1);
		guide_key(&g, m, 27, t + k);
		if (guide_tick(&g, t + k))
			break;
	}
	CHECK(g.res[5].capped);
	CHECK_EQ(g.cur, 6);
	CHECK_EQ(g.res[5].end_ms, t + GUIDE_MAX_MS);

	/* two bytes at once; the constant bits of byte 15 are not news */
	t = g.res[6].begin_ms;
	msg(m, 3, 0x04);
	m[0] = 0x10;
	m[15] = 0x06;
	guide_key(&g, m, 27, t + 1);
	guide_tick(&g, t + 1 + GUIDE_QUIET_MS);
	guide_signature(&g.res[6], sig, sizeof(sig));
	CHECK_STR(sig, "b0 0x10, b3 0x04, b15 0x04");

	/* to the end: every prompt idles out, then nothing more happens */
	while (!guide_done(&g))
		guide_tick(&g, g.res[g.cur].begin_ms + GUIDE_IDLE_MS);
	CHECK_EQ(g.cur, guide_nprompts);
	CHECK_EQ(guide_tick(&g, 999999), 0);
	guide_key(&g, m, 27, 999999);
	CHECK_EQ(guide_left_ms(&g, 999999), 0);

	/* key_bits */
	msg(m, 2, 0x80);
	CHECK_EQ(key_bits(m, 27, 2), 0x80);
	CHECK_EQ(key_bits(m, 27, 14), 0);
	CHECK_EQ(key_bits(m, 27, 16), 0);
	CHECK_EQ(key_bits(m, 10, 12), 0);
	CHECK_DONE();
}

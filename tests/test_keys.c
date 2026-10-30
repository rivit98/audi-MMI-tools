/*
 * test_keys.c - src/hello7/keys.c on the ch8 messages logged on 2026-10-21
 * (docs/platform-notes.md section 8), plus made-up unknown bits.
 */
#include "check.h"
#include "keys.h"

/* a 27-byte message with the constant bytes set and one byte changed */
static void msg(unsigned char *m, int byte, int value)
{
	memset(m, 0, 27);
	m[14] = 0x01;
	m[15] = 0x02;
	m[17] = 0x01;
	if (byte >= 0)
		m[byte] = (unsigned char)value;
}

static void one(int byte, int value, int ctl, int amount, const char *text)
{
	unsigned char m[27];
	struct key_hit h[KEY_MAX_HITS];
	char buf[128];
	int n;

	msg(m, byte, value);
	n = key_decode(m, 27, h, KEY_MAX_HITS);
	CHECK_EQ(n, 1);
	CHECK_EQ(h[0].ctl, ctl);
	CHECK_EQ(h[0].amount, amount);
	key_describe(h, n, buf, sizeof(buf));
	CHECK_STR(buf, text);
}

int main(void)
{
	unsigned char m[27];
	struct key_hit h[KEY_MAX_HITS];
	char buf[128];
	int n;

	one(6, 1, K_MAIN_RIGHT, 1, "main knob right x1");
	one(7, 1, K_MAIN_LEFT, 1, "main knob left x1");
	one(4, 1, K_VOL_UP, 1, "volume up x1");
	one(5, 2, K_VOL_DOWN, 2, "volume down x2");    /* a fast turn */
	one(3, 0x04, K_MENU, 1, "MENU");
	one(1, 0x02, K_SOFT_TL, 1, "top-left soft key");
	one(1, 0x40, K_B1_40, 1, "button b1 0x40");
	one(-1, 0, K_RELEASE, 0, "release");
	one(9, 0x10, K_UNKNOWN, 1, "unknown b9 0x10");
	one(14, 0x03, K_UNKNOWN, 1, "unknown b14 0x02");  /* a constant changed */

	/* 2026-10-25: state bytes 16 = 0x03, 26 = 0x11, 23/25 changing: no key */
	msg(m, 6, 1);
	m[16] = 0x03;
	m[23] = 0x12;
	m[25] = 0x0f;
	m[26] = 0x11;
	n = key_decode(m, 27, h, KEY_MAX_HITS);
	CHECK_EQ(n, 1);
	key_describe(h, n, buf, sizeof(buf));
	CHECK_STR(buf, "main knob right x1");
	msg(m, -1, 0);
	m[16] = 0x03;
	m[26] = 0x11;
	CHECK_EQ(key_decode(m, 27, h, KEY_MAX_HITS), 1);
	CHECK_EQ(h[0].ctl, K_RELEASE);

	/* two buttons and an unknown bit in the same byte at once */
	msg(m, 1, 0x02 | 0x40 | 0x01);
	m[3] = 0x04;
	n = key_decode(m, 27, h, KEY_MAX_HITS);
	CHECK_EQ(n, 4);
	key_describe(h, n, buf, sizeof(buf));
	CHECK_STR(buf, "MENU + top-left soft key + button b1 0x40 + unknown b1 0x01");

	/* too short to hold the knob bytes */
	n = key_decode(m, 4, h, KEY_MAX_HITS);
	CHECK_EQ(n, 1);
	CHECK_EQ(h[0].ctl, K_UNKNOWN);
	CHECK_EQ(h[0].byte, -1);

	/* out[] is never overrun */
	memset(m, 0xff, sizeof(m));
	n = key_decode(m, 27, h, 3);
	CHECK_EQ(n, 3);
	CHECK_DONE();
}

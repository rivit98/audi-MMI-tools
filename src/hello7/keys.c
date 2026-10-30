/*
 * keys.c - see keys.h.
 */
#include <stdio.h>
#include <string.h>

#include "keys.h"

#define MSG_BYTES 32            /* ch8 messages are 27 */
#define STATE_FIRST 16          /* bytes 16-26: state, not controls (below) */
#define STATE_LAST  26

/* The known controls: a knob takes the whole byte (its value = detents),
 * a button one bit. */
static const struct {
	int byte, mask, ctl, knob;
} known[] = {
	{ 6, 0xff, K_MAIN_RIGHT, 1 },
	{ 7, 0xff, K_MAIN_LEFT,  1 },
	{ 4, 0xff, K_VOL_UP,     1 },
	{ 5, 0xff, K_VOL_DOWN,   1 },
	{ 3, 0x04, K_MENU,       0 },
	{ 1, 0x02, K_SOFT_TL,    0 },
	{ 1, 0x40, K_B1_40,      0 },
	{ 1, 0x80, K_B1_80,      0 },
};
#define NKNOWN ((int)(sizeof(known) / sizeof(known[0])))

/* bytes that are always set: anything else in them is news */
static int constant(int i)
{
	return i == 14 ? 0x01 : i == 15 ? 0x02 : 0;
}

/* Bytes 16-26 hold some state of the unit, not controls [inferred]: byte
 * 16 was 0x00 on 2026-10-21 and 0x03 in every message on 2026-10-25, byte
 * 26 0x11 all of 25.10, and bytes 23/25 changed with no control touched.
 * Decoding them as key bits made every message "unknown" (112 of them on
 * 25.10), so they are left to the raw log. */
static int is_state(int i)
{
	return i >= STATE_FIRST && i <= STATE_LAST;
}

static int add(struct key_hit *out, int n, int max, int ctl, int amount,
               int byte, int bits)
{
	if (n >= max)
		return n;
	out[n].ctl = ctl;
	out[n].amount = amount;
	out[n].byte = byte;
	out[n].bits = bits;
	return n + 1;
}

int key_decode(const unsigned char *buf, int n, struct key_hit *out, int max)
{
	int left[MSG_BYTES], i, hits = 0;

	if (n < 8)
		return add(out, 0, max, K_UNKNOWN, 1, -1, 0);
	if (n > MSG_BYTES)
		n = MSG_BYTES;
	for (i = 0; i < n; i++)
		left[i] = is_state(i) ? 0 : buf[i] ^ constant(i);
	for (i = 0; i < NKNOWN; i++) {
		int b = known[i].byte, v = left[b] & known[i].mask;

		if (v == 0)
			continue;
		hits = add(out, hits, max, known[i].ctl, known[i].knob ? v : 1, b, v);
		left[b] &= ~known[i].mask;
	}
	for (i = 0; i < n; i++)
		if (left[i] != 0)
			hits = add(out, hits, max, K_UNKNOWN, 1, i, left[i]);
	if (hits == 0)
		hits = add(out, 0, max, K_RELEASE, 0, 0, 0);
	return hits;
}

const char *key_name(int ctl)
{
	static const char *const names[K_NCTL] = {
		"main knob right", "main knob left", "volume up", "volume down",
		"MENU", "top-left soft key", "button b1 0x40", "button b1 0x80",
		"unknown", "release",
	};

	return ctl >= 0 && ctl < K_NCTL ? names[ctl] : "?";
}

void key_describe(const struct key_hit *hits, int n, char *buf, int size)
{
	int i, len = 0;

	buf[0] = '\0';
	for (i = 0; i < n && len < size; i++) {
		const struct key_hit *h = &hits[i];
		const char *sep = i ? " + " : "";

		if (h->ctl == K_UNKNOWN && h->byte < 0)
			len += snprintf(buf + len, size - len, "%sshort message", sep);
		else if (h->ctl == K_UNKNOWN)
			len += snprintf(buf + len, size - len, "%sunknown b%d 0x%02x", sep,
			                h->byte, h->bits);
		else if (h->ctl <= K_VOL_DOWN)
			len += snprintf(buf + len, size - len, "%s%s x%d", sep,
			                key_name(h->ctl), h->amount);
		else
			len += snprintf(buf + len, size - len, "%s%s", sep, key_name(h->ctl));
	}
}

/*
 * keys.h - decode /dev/ipc/ch8 messages: which knob or button each one
 * reports (docs/platform-notes.md section 8, decoded on 2026-10-21).
 *
 * A message is the state of the controls: knob bytes carry detents since
 * the last message (6/7 main knob right/left, 4/5 volume up/down), buttons
 * are bits in bytes 1 and 3 while pressed, and an all-zero message follows
 * a release. Bytes 14, 15 and 17 are always 01 02 .. 01. Any other set bit
 * is reported as unknown, with its byte and bits, so a new button can be
 * named from one press.
 */
#ifndef HELLO7_KEYS_H
#define HELLO7_KEYS_H

enum key_ctl {
	K_MAIN_RIGHT, K_MAIN_LEFT, K_VOL_UP, K_VOL_DOWN,
	K_MENU, K_SOFT_TL, K_B1_40, K_B1_80,
	K_UNKNOWN, K_RELEASE,
	K_NCTL
};

struct key_hit {
	int ctl;                /* enum key_ctl */
	int amount;             /* detents for a knob, 1 for a button */
	int byte, bits;         /* K_UNKNOWN: where (byte -1: short message) */
};

#define KEY_MAX_HITS 16

/* Decode buf[0..n). Returns the number of hits written to out (at most
 * max): K_RELEASE alone if no control is active. */
int key_decode(const unsigned char *buf, int n, struct key_hit *out, int max);

const char *key_name(int ctl);

/* "main knob right x2 + MENU" into buf. */
void key_describe(const struct key_hit *hits, int n, char *buf, int size);

#endif

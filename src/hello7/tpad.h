/*
 * tpad.h - the touch pad's and the steering wheel's parts of /dev/ipc/ch8,
 * as MMI3GApplication's KeyPanel reads them (src/common/buttons/README.md
 * "Touch pad" and "Steering wheel"; addresses there).
 *
 * Touch pad: byte 14 = 0x02, byte 27 = a subfunction, its data from byte
 * 28 on. The pad recognises on its own (virtual keys, gestures,
 * handwriting) in the mode the HMI sets: 22 00 00 00 00 00 02 00 C4 <mode>
 * written to ch8, echoed back as subfunction C4.
 *
 * Steering wheel (MFL): byte 14 = 0x01, byte 16 = source (0 or 3), bytes
 * 23/24 = two key codes, byte 25 = their events (low / high nibble),
 * byte 26 = variant << 4 | brand. Codes 0x06 and 0x12 are rollers: the
 * event nibble is signed ticks (0x1 = +1, 0xF = -1).
 *
 * Pure (no car APIs), tested on the host (tests/test_tpad.c).
 */
#ifndef HELLO7_TPAD_H
#define HELLO7_TPAD_H

#include "types.h"

#define CH8_KIND_BYTE  14
#define CH8_KIND_KEYS  0x01
#define CH8_KIND_EXT   0x02     /* touch pad */
#define TPAD_SUB_BYTE  27

/* The subfunction of a touch pad message, or -1. */
int tpad_sub(const u8 *m, int n);

/* 1 for what a finger makes (press, move over a key, gesture, character,
 * coordinates); 0 for the pad's status answers (mode, language, setting). */
int tpad_is_user(int sub);

/* The mode in a C4 echo, or -1. */
int tpad_mode_echo(const u8 *m, int n);

/* "press x=132 y=198", "approached KEY6", "mode 6", "sub 0x99 ...". */
void tpad_describe(const u8 *m, int n, char *buf, int size);

/* The HMI's "set recognizer mode" command into out (10 bytes). Returns the
 * length, or 0 if size is too small. */
int tpad_mode_cmd(int mode, u8 *out, int size);

/* The pad's recognizer modes MMI3GApplication passes on (0x8226994). */
extern const int tpad_modes[];
extern const int tpad_nmodes;

/* ---- steering wheel ---- */

#define MFL_MAX 2

struct mfl_hit {
	int code;               /* MFL key code, 1..0xF1 */
	int roller;             /* code 0x06 or 0x12 */
	int ticks;              /* roller: signed ticks */
	int event;              /* key: the raw event nibble */
};

/* Wheel keys in a message: 0 for none (a release, a panel message, or a
 * source the MMI doesn't read for the wheel). */
int mfl_decode(const u8 *m, int n, struct mfl_hit *out, int max);

/* "wheel roller 0x12 +1", "wheel key 0x05 event 1"; "" if none. */
void mfl_describe(const u8 *m, int n, char *buf, int size);

#endif

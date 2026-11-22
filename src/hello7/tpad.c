/*
 * tpad.c - see tpad.h. Addresses: MMI3GApplication (P1001, rev 51).
 */
#include <stdio.h>
#include <string.h>

#include "tpad.h"
#include "types.h"

/* 0x8226994: the modes passed on to the pad (others become 0) */
const int tpad_modes[] = { 6, 9, 10, 11, 12, 13, 16, 17, 18 };
const int tpad_nmodes = (int)(sizeof(tpad_modes) / sizeof(tpad_modes[0]));

int tpad_sub(const u8 *m, int n)
{
	if (n <= TPAD_SUB_BYTE || m[CH8_KIND_BYTE] != CH8_KIND_EXT)
		return -1;
	return m[TPAD_SUB_BYTE];
}

int tpad_is_user(int sub)
{
	switch (sub) {
	case 0x30: case 0x31: case 0x38: case 0x39: case 0x3d: case 0x3e:
	case 0x48: case 0xc5: case 0xc6: case 0xc7: case 0xca: case 0xcb:
	case 0xd3:
		return 1;
	default:
		return 0;
	}
}

int tpad_mode_echo(const u8 *m, int n)
{
	if (tpad_sub(m, n) != 0xc4 || n < 29)
		return -1;
	return m[28];
}

static const char *const dirs[] = {
	"CENTER", "N", "NE", "E", "SE", "S", "SW", "W", "NW",
};

/* Directions: virtual keys 0x50..0x58 (0x82279A0), scroll 1..8 (0x8227958)
 * and border status 0..8 all map to the same CENTER, N .. NW codes. */
static const char *dir_name(int v)
{
	return v >= 0 && v < 9 ? dirs[v] : "?";
}

/* 0x82279A0: the pad's virtual keys (radio presets, eight directions) */
static const char *key_name(int code)
{
	switch (code) {
	case 0x37: return "KEY1";
	case 0x3d: return "KEY2";
	case 0x39: return "KEY3";
	case 0x3a: return "KEY4";
	case 0x3b: return "KEY5";
	case 0x3c: return "KEY6";
	default:
		return code >= 0x50 && code <= 0x58 ? dirs[code - 0x50] : NULL;
	}
}

static int at(const u8 *m, int n, int i)
{
	return i < n ? m[i] : -1;
}

void tpad_describe(const u8 *m, int n, char *buf, int size)
{
	int sub = tpad_sub(m, n), a = at(m, n, 28), b = at(m, n, 29), len, i;
	const char *k;

	buf[0] = '\0';
	if (sub < 0)
		return;
	switch (sub) {
	case 0x30: case 0x31: case 0x3d: case 0x3e:
		k = key_name(a);
		snprintf(buf, size, "pad key %s %s",
		         sub == 0x30 ? "pressed" : sub == 0x31 ? "released" :
		         sub == 0x3d ? "approached" : "abandoned", k ? k : "?");
		if (!k)
			snprintf(buf + strlen(buf), size - (int)strlen(buf), " (0x%02x)", a & 0xff);
		break;
	case 0x38:
		snprintf(buf, size, "pad scroll %s", a >= 1 && a <= 8 ? dir_name(a) : "?");
		break;
	case 0x39:
		snprintf(buf, size, "pad border %s", dir_name(a));
		break;
	case 0x48:
		snprintf(buf, size, "pad slider %d %+d", a, b >= 0 ? (int)(i8)b : 0);
		break;
	case 0xc1: case 0xc8:
		snprintf(buf, size, "pad setting %d = 0x%02x%02x%02x%02x", at(m, n, 30),
		         at(m, n, 31) & 0xff, at(m, n, 32) & 0xff, at(m, n, 33) & 0xff,
		         at(m, n, 34) & 0xff);
		break;
	case 0xc2:
		len = snprintf(buf, size, "pad language \"");
		for (i = 28; i < 33 && i < n && m[i] && len < size - 2; i++)
			buf[len++] = (m[i] >= 0x20 && m[i] < 0x7f) ? (char)m[i] : '.';
		snprintf(buf + len, size - len, "\"");
		break;
	case 0xc4:
		snprintf(buf, size, "pad mode %d", a);
		break;
	case 0xc5:
		snprintf(buf, size, "pad abs x=%d y=%d", a, b);
		break;
	case 0xc6:
		snprintf(buf, size, "pad rel %+d %+d", a >= 0 ? (int)(i8)a : 0,
		         b >= 0 ? (int)(i8)b : 0);
		break;
	case 0xc7:
		len = snprintf(buf, size, "pad char%s \"", b == 1 ? "" : " (not UTF-8)");
		for (i = 30; i < n && m[i] && len < size - 2; i++)
			buf[len++] = (m[i] >= 0x20 && m[i] < 0x7f) ? (char)m[i] : '?';
		snprintf(buf + len, size - len, "\"");
		break;
	case 0xca:
		snprintf(buf, size, "pad press x=%d y=%d", a, b);
		break;
	case 0xcb:
		snprintf(buf, size, "pad release");
		break;
	case 0xd3:
		snprintf(buf, size, "pad %s", a == 0 ? "tap" : a == 1 ? "double tap" : "gesture ?");
		break;
	default:
		snprintf(buf, size, "pad sub 0x%02x", sub);
		break;
	}
}

int tpad_mode_cmd(int mode, u8 *out, int size)
{
	if (size < 10)
		return 0;
	memset(out, 0, 10);
	out[0] = 0x22;          /* 0x822695C: every write to the panel starts so */
	out[6] = CH8_KIND_EXT;
	out[8] = 0xc4;
	out[9] = (u8)mode;
	return 10;
}

/* ---- steering wheel (0x822B1B4, 0x822B0FC) ---- */

static int is_roller(int code)
{
	return code == 0x06 || code == 0x12;
}

int mfl_decode(const u8 *m, int n, struct mfl_hit *out, int max)
{
	int i, hits = 0;

	/* byte 16: 1 and 2 go to the panel handler only (0x822C038) */
	if (n < 27 || m[CH8_KIND_BYTE] != CH8_KIND_KEYS || m[16] == 1 || m[16] == 2)
		return 0;
	for (i = 0; i < MFL_MAX && hits < max; i++) {
		int code = m[23 + i], ev = i == 0 ? m[25] & 0x0f : m[25] >> 4;

		if (code == 0 || code == 0x22)  /* 0x22: NO_KEY */
			continue;
		out[hits].code = code;
		out[hits].roller = is_roller(code);
		out[hits].ticks = ev > 8 ? ev - 16 : ev;
		out[hits].event = ev;
		hits++;
	}
	return hits;
}

void mfl_describe(const u8 *m, int n, char *buf, int size)
{
	struct mfl_hit h[MFL_MAX];
	int nh = mfl_decode(m, n, h, MFL_MAX), i, len = 0;

	buf[0] = '\0';
	for (i = 0; i < nh && len < size; i++) {
		const char *sep = i ? " + " : "";

		if (h[i].roller)
			len += snprintf(buf + len, size - len, "%swheel roller 0x%02x %+d", sep,
			                h[i].code, h[i].ticks);
		else
			len += snprintf(buf + len, size - len, "%swheel key 0x%02x event %d", sep,
			                h[i].code, h[i].event);
	}
}

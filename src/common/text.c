/*
 * text.c - UTF-8 decoding, word wrap and bitmap-font drawing (see text.h).
 */
#include "types.h"
#include <string.h>

#include "text.h"

#define REPLACEMENT 0xFFFD

u32 utf8_next(const char **s, const char *end)
{
	const unsigned char *p = (const unsigned char *)*s;
	const unsigned char *e = (const unsigned char *)end;
	u32 cp, min;
	int n, i;

	if ((e != NULL && p >= e) || *p == 0)
		return 0;
	if (p[0] < 0x80) {
		*s = (const char *)p + 1;
		return p[0];
	}
	if ((p[0] & 0xE0) == 0xC0) {
		n = 1; cp = p[0] & 0x1F; min = 0x80;
	} else if ((p[0] & 0xF0) == 0xE0) {
		n = 2; cp = p[0] & 0x0F; min = 0x800;
	} else if ((p[0] & 0xF8) == 0xF0) {
		n = 3; cp = p[0] & 0x07; min = 0x10000;
	} else {
		*s = (const char *)p + 1;
		return REPLACEMENT;
	}
	for (i = 1; i <= n; i++) {
		if ((e != NULL && p + i >= e) || (p[i] & 0xC0) != 0x80) {
			*s = (const char *)p + 1;
			return REPLACEMENT;
		}
		cp = (cp << 6) | (p[i] & 0x3F);
	}
	*s = (const char *)p + 1 + n;
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return REPLACEMENT;
	return cp;
}

int utf8_count(const char *s, int nbytes)
{
	const char *end = s + (nbytes < 0 ? (int)strlen(s) : nbytes);
	int n = 0;

	while (utf8_next(&s, end) != 0)
		n++;
	return n;
}

int font_glyph(const struct font *f, u32 cp)
{
	int lo = 0, hi = f->nmap - 1;

	if (cp > 0xFFFF)
		return f->fallback;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (f->map_cp[mid] == cp)
			return f->map_glyph[mid];
		if (f->map_cp[mid] < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return f->fallback;
}

void canvas_fill(struct canvas *c, int x, int y, int w, int h, u32 color)
{
	int x1 = x + w, y1 = y + h, i;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > c->w) x1 = c->w;
	if (y1 > c->h) y1 = c->h;
	for (; y < y1; y++) {
		u32 *row = c->px + y * c->stride;

		for (i = x; i < x1; i++)
			row[i] = color;
	}
}

static void draw_glyph(struct canvas *c, const struct font *f, int x, int y,
                       int glyph, u32 color)
{
	const u8 *g = f->bits + glyph * f->h * f->row_bytes;
	int gy, gx;

	if (x >= c->w || y >= c->h || x + f->w <= 0 || y + f->h <= 0)
		return;
	for (gy = 0; gy < f->h; gy++, g += f->row_bytes) {
		u32 *row;

		if (y + gy < 0 || y + gy >= c->h)
			continue;
		row = c->px + (y + gy) * c->stride;
		for (gx = 0; gx < f->w; gx++) {
			if (x + gx < 0 || x + gx >= c->w)
				continue;
			if (g[gx >> 3] & (0x80 >> (gx & 7)))
				row[x + gx] = color;
		}
	}
}

int text_draw(struct canvas *c, const struct font *f, int x, int y,
              const char *s, int nbytes, u32 color)
{
	const char *end = s + (nbytes < 0 ? (int)strlen(s) : nbytes);
	u32 cp;

	while ((cp = utf8_next(&s, end)) != 0) {
		if (cp == '\r')
			continue;
		if (cp == '\t')
			cp = ' ';
		draw_glyph(c, f, x, y, font_glyph(f, cp), color);
		x += f->w;
	}
	return x;
}

static int is_space(char ch)
{
	return ch == ' ' || ch == '\t';
}

/* Wrap one paragraph [s, end) (no '\n' inside). */
static int wrap_para(const char *s, const char *end, int cols,
                     text_line_cb cb, void *arg)
{
	int lines = 0;

	if (s == end) {
		cb(s, 0, arg);
		return 1;
	}
	while (s < end) {
		const char *p = s, *brk = NULL;
		int n = 0;

		while (p < end && n < cols) {
			if (is_space(*p) && p > s)
				brk = p;
			utf8_next(&p, end);
			n++;
		}
		if (p >= end) {                     /* the rest fits */
			cb(s, (int)(end - s), arg);
			return lines + 1;
		}
		if (is_space(*p)) {                 /* break exactly at a space */
			cb(s, (int)(p - s), arg);
			s = p + 1;
		} else if (brk != NULL) {           /* back to the last space */
			cb(s, (int)(brk - s), arg);
			s = brk + 1;
		} else {                            /* one long word: split it */
			cb(s, (int)(p - s), arg);
			s = p;
		}
		lines++;
	}
	return lines;
}

int text_wrap(const char *s, int cols, text_line_cb cb, void *arg)
{
	int lines = 0;

	if (cols < 1)
		cols = 1;
	for (;;) {
		const char *nl = strchr(s, '\n');
		const char *end = nl != NULL ? nl : s + strlen(s);

		if (end > s && end[-1] == '\r')
			end--;
		lines += wrap_para(s, end, cols, cb, arg);
		if (nl == NULL)
			return lines;
		s = nl + 1;
	}
}

/*
 * layout.c - pngondemand's layout language (see README.md).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_big.h"
#include "font_small.h"
#include "layout.h"

#define MAX_LINE    4096
#define MAX_COLOURS 32

struct colour {
	char     name[16];
	uint32_t rgb;
};

struct state {
	struct layout_result *r;
	int           w, h;
	int           margin, y, bottom;
	struct colour colours[MAX_COLOURS];
	int           ncolours;
	/* para's line callback */
	const struct font *pf;
	uint32_t      pc;
};

static int fail(struct state *st, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(st->r->err, sizeof(st->r->err), fmt, ap);
	va_end(ap);
	return -1;
}

/* Next space-separated word of *s, NUL-terminated in place. The single
 * space after it is consumed, so *s is then where TEXT would start. */
static char *word(char **s)
{
	char *p = *s, *w;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == 0) {
		*s = p;
		return NULL;
	}
	w = p;
	while (*p != 0 && *p != ' ' && *p != '\t')
		p++;
	if (*p != 0)
		*p++ = 0;
	*s = p;
	return w;
}

static int number(struct state *st, char **s, const char *what, int *out)
{
	char *w = word(s), *end;
	long v;

	if (w == NULL)
		return fail(st, "missing %s", what);
	v = strtol(w, &end, 10);
	if (*end != 0 || end == w || v < -100000 || v > 100000)
		return fail(st, "bad %s \"%s\"", what, w);
	*out = (int)v;
	return 0;
}

static int hexdigit(int ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

/* RRGGBB / #RRGGBB, or -1. */
static long parse_hex(const char *w)
{
	long v = 0;
	int i;

	if (*w == '#')
		w++;
	for (i = 0; i < 6; i++) {
		int d = hexdigit((unsigned char)w[i]);

		if (d < 0)
			return -1;
		v = v << 4 | d;
	}
	return w[6] == 0 ? v : -1;
}

static int colour(struct state *st, char **s, uint32_t *out)
{
	char *w = word(s);
	long v;
	int i;

	if (w == NULL)
		return fail(st, "missing colour");
	for (i = 0; i < st->ncolours; i++)
		if (strcmp(st->colours[i].name, w) == 0) {
			*out = st->colours[i].rgb;
			return 0;
		}
	if ((v = parse_hex(w)) < 0)
		return fail(st, "unknown colour \"%s\"", w);
	*out = 0xFF000000u | (uint32_t)v;
	return 0;
}

static int font(struct state *st, char **s, const struct font **out)
{
	char *w = word(s);

	if (w == NULL)
		return fail(st, "missing font");
	if (strcmp(w, "small") == 0)
		*out = &font_small;
	else if (strcmp(w, "big") == 0)
		*out = &font_big;
	else
		return fail(st, "unknown font \"%s\" (small or big)", w);
	return 0;
}

static int no_more(struct state *st, char **s)
{
	char *w = word(s);

	return w != NULL ? fail(st, "unexpected \"%s\"", w) : 0;
}

static int canvas(struct state *st)
{
	struct canvas *c = &st->r->canvas;

	if (c->px != NULL)
		return 0;
	c->px = malloc((size_t)st->w * st->h * 4);
	if (c->px == NULL)
		return fail(st, "out of memory");
	c->w = c->stride = st->w;
	c->h = st->h;
	canvas_fill(c, 0, 0, c->w, c->h, 0xFF000000u);
	return 0;
}

/* One flow line of s[0..nbytes) at the margin, cut to fit. */
static void flow_line(struct state *st, const struct font *f, uint32_t rgb,
                      const char *s, int nbytes)
{
	struct canvas *c = &st->r->canvas;
	int cols = (c->w - 2 * st->margin) / f->w;
	const char *p = s, *q, *end = s + nbytes;
	int n = 0;

	if (st->y < 0 || st->y + f->h > st->bottom) {
		st->r->dropped++;
	} else {
		while (n < cols && utf8_next(&p, end) != 0)
			n++;
		q = p;
		if (utf8_next(&q, end) != 0)
			st->r->cut++;
		text_draw(c, f, st->margin, st->y, s, (int)(p - s), rgb);
	}
	st->y += f->h;
}

static void para_line(const char *line, int nbytes, void *arg)
{
	struct state *st = arg;

	flow_line(st, st->pf, st->pc, line, nbytes);
}

/* "30 s" -> "30 s": text_wrap only breaks at plain spaces. */
static char *keep_numbers(const char *s)
{
	char *out = malloc(strlen(s) * 2 + 1), *o = out;

	if (out == NULL)
		return NULL;
	for (; *s; s++) {
		if (*s == ' ' && o > out && isdigit((unsigned char)o[-1]) &&
		    isalnum((unsigned char)s[1])) {
			*o++ = (char)0xC2;
			*o++ = (char)0xA0;
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
	return out;
}

static int command(struct state *st, char *s)
{
	struct canvas *c = &st->r->canvas;
	const struct font *f;
	uint32_t rgb, fg;
	int x, y, w, h;
	char *cmd = word(&s), *name;

	if (cmd == NULL || cmd[0] == '#')
		return 0;

	if (strcmp(cmd, "size") == 0) {
		if (number(st, &s, "width", &w) || number(st, &s, "height", &h) ||
		    no_more(st, &s))
			return -1;
		if (c->px != NULL)
			return fail(st, "size must come before anything is drawn");
		if (w < 1 || h < 1 || w > LAYOUT_MAX_W || h > LAYOUT_MAX_H)
			return fail(st, "size %dx%d out of range (max %dx%d)",
			            w, h, LAYOUT_MAX_W, LAYOUT_MAX_H);
		st->w = w;
		st->h = st->bottom = h;
		return 0;
	}
	if (strcmp(cmd, "colour") == 0 || strcmp(cmd, "color") == 0) {
		if ((name = word(&s)) == NULL)
			return fail(st, "missing colour name");
		if (strlen(name) >= sizeof(st->colours[0].name))
			return fail(st, "colour name \"%s\" too long", name);
		if (parse_hex(name) >= 0)
			return fail(st, "colour name \"%s\" looks like a colour", name);
		if (colour(st, &s, &rgb) || no_more(st, &s))
			return -1;
		for (x = 0; x < st->ncolours; x++)
			if (strcmp(st->colours[x].name, name) == 0)
				break;
		if (x == MAX_COLOURS)
			return fail(st, "more than %d colours", MAX_COLOURS);
		if (x == st->ncolours)
			st->ncolours++;
		strcpy(st->colours[x].name, name);
		st->colours[x].rgb = rgb;
		return 0;
	}
	if (strcmp(cmd, "margin") == 0)
		return number(st, &s, "margin", &st->margin) || no_more(st, &s) ? -1 : 0;
	if (strcmp(cmd, "at") == 0)
		return number(st, &s, "y", &st->y) || no_more(st, &s) ? -1 : 0;
	if (strcmp(cmd, "gap") == 0) {
		if (number(st, &s, "gap", &y) || no_more(st, &s))
			return -1;
		st->y += y;
		return 0;
	}

	if (strcmp(cmd, "bg") == 0) {
		if (colour(st, &s, &rgb) || no_more(st, &s) || canvas(st))
			return -1;
		canvas_fill(c, 0, 0, c->w, c->h, rgb);
		return 0;
	}
	if (strcmp(cmd, "rect") == 0) {
		if (number(st, &s, "x", &x) || number(st, &s, "y", &y) ||
		    number(st, &s, "width", &w) || number(st, &s, "height", &h) ||
		    colour(st, &s, &rgb) || no_more(st, &s) || canvas(st))
			return -1;
		canvas_fill(c, x, y, w, h, rgb);
		return 0;
	}
	if (strcmp(cmd, "text") == 0) {
		if (number(st, &s, "x", &x) || number(st, &s, "y", &y) ||
		    font(st, &s, &f) || colour(st, &s, &rgb) || canvas(st))
			return -1;
		text_draw(c, f, x, y, s, -1, rgb);
		return 0;
	}
	if (strcmp(cmd, "line") == 0) {
		if (font(st, &s, &f) || colour(st, &s, &rgb) || canvas(st))
			return -1;
		flow_line(st, f, rgb, s, (int)strlen(s));
		return 0;
	}
	if (strcmp(cmd, "para") == 0) {
		char *t;

		if (font(st, &s, &f) || colour(st, &s, &rgb) || canvas(st))
			return -1;
		if ((t = keep_numbers(s)) == NULL)
			return fail(st, "out of memory");
		st->pf = f;
		st->pc = rgb;
		text_wrap(t, (c->w - 2 * st->margin) / f->w, para_line, st);
		free(t);
		return 0;
	}
	if (strcmp(cmd, "footer") == 0) {
		if (number(st, &s, "height", &h) || font(st, &s, &f) ||
		    colour(st, &s, &rgb) || colour(st, &s, &fg) || canvas(st))
			return -1;
		if (h < 0 || h > c->h)
			return fail(st, "footer height %d out of range", h);
		canvas_fill(c, 0, c->h - h, c->w, h, rgb);
		text_draw(c, f, st->margin, c->h - h + (h - f->h) / 2, s, -1, fg);
		st->bottom = c->h - h;
		return 0;
	}
	return fail(st, "unknown command \"%s\"", cmd);
}

int layout_render(const char *src, struct layout_result *r)
{
	struct state st;
	char line[MAX_LINE];
	int n = 0;

	memset(r, 0, sizeof(*r));
	memset(&st, 0, sizeof(st));
	st.r = r;
	st.w = 800;
	st.h = st.bottom = 480;
	st.margin = 24;
	st.y = 20;

	while (*src != 0) {
		const char *nl = strchr(src, '\n');
		size_t len = nl != NULL ? (size_t)(nl - src) : strlen(src);

		n++;
		if (len >= sizeof(line)) {
			fail(&st, "line longer than %d bytes", MAX_LINE - 1);
			goto err;
		}
		memcpy(line, src, len);
		if (len > 0 && line[len - 1] == '\r')
			len--;
		line[len] = 0;
		if (command(&st, line) != 0)
			goto err;
		src = nl != NULL ? nl + 1 : src + strlen(src);
	}
	if (canvas(&st) == 0)
		return 0;
	n = 0;
err:
	r->err_line = n;
	free(r->canvas.px);
	r->canvas.px = NULL;
	return -1;
}

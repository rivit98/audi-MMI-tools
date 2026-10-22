/*
 * label.c - full-screen instruction text between hello6's steps, drawn by
 * the CPU with common/screen.c (cid 40) and the Terminus fonts.
 */
#include <string.h>

#include "font_big.h"
#include "font_small.h"
#include "hello6.h"
#include "screen.h"
#include "text.h"

#define MARGIN   20
#define C_BG     0xff101828u
#define C_TITLE  0xffffd040u
#define C_TEXT   0xffffffffu
#define C_FOOT   0xff90a0b0u
#define C_BAR    0xff2a3a58u

struct wrap_ctx {
	struct canvas *c;
	int y, dropped;
};

static int last_dropped;

static void draw_line(const char *line, int nbytes, void *arg)
{
	struct wrap_ctx *w = arg;

	if (w->y + font_big.h <= SCREEN_H - 2 * font_small.h)
		text_draw(w->c, &font_big, MARGIN, w->y, line, nbytes, C_TEXT);
	else
		w->dropped++;
	w->y += font_big.h + 4;
}

static int is_open;

int label_show(const char *title, const char *body, const char *footer)
{
	struct canvas *c;
	struct wrap_ctx w;

	if (!is_open) {
		if (screen_open() != 0) {
			screen_close();
			return -1;
		}
		is_open = 1;
	}
	c = screen_canvas();
	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	canvas_fill(c, 0, 0, c->w, font_big.h + 16, C_BAR);
	text_draw(c, &font_big, MARGIN, 8, title, -1, C_TITLE);
	w.c = c;
	w.y = font_big.h + 32;
	w.dropped = 0;
	text_wrap(body, (c->w - 2 * MARGIN) / font_big.w, draw_line, &w);
	last_dropped = w.dropped;
	if (footer != NULL)
		text_draw(c, &font_small, MARGIN, c->h - font_small.h - 8, footer, -1, C_FOOT);
	return screen_present();
}

int label_show_list(const char *title, const char *const *lines, int n)
{
	struct canvas *c;
	int i, y;

	if (label_show(title, "", NULL) != 0)
		return -1;
	c = screen_canvas();
	y = font_big.h + 24;
	for (i = 0; i < n && y + font_small.h <= c->h; i++, y += font_small.h + 1)
		text_draw(c, &font_small, MARGIN, y, lines[i], -1, C_TEXT);
	last_dropped = n - i;
	return screen_present();
}

int label_dropped(void)
{
	return last_dropped;
}

void label_hide(void)
{
	if (is_open)
		screen_close();
	is_open = 0;
}

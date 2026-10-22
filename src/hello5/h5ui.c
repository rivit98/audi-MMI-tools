/*
 * h5ui.c - hello5's screen (see h5ui.h). Each group of chimes has its own
 * background colour and a huge group number, so the group is readable on
 * a blurry video; the white PLAYING box is up while a chime sounds.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "font_big.h"
#include "font_small.h"
#include "log.h"
#include "screen.h"
#include "text.h"
#include "h5ui.h"

#define MARGIN   20
#define C_GAP    0xff202428u
#define C_TEXT   0xffffffffu
#define C_DIM    0xffa0a8b0u
#define C_TITLE  0xffffd040u
#define C_BOX    0xffffffffu
#define C_BOXTXT 0xff000000u

/* one per group: blue, green, purple, brown, teal, red */
static const uint32_t group_bg[] = {
	0xff1838a0u, 0xff187028u, 0xff682088u, 0xff805010u, 0xff106870u, 0xff901818u,
};

static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static int ui_ok;

/* Text with every font pixel drawn as a scale x scale block. */
static int draw_scaled(struct canvas *c, const struct font *f, int x, int y,
                       int scale, const char *s, uint32_t color)
{
	const char *end = s + strlen(s);
	uint32_t cp;

	while ((cp = utf8_next(&s, end)) != 0) {
		const uint8_t *g = f->bits + font_glyph(f, cp) * f->h * f->row_bytes;
		int gx, gy;

		for (gy = 0; gy < f->h; gy++, g += f->row_bytes)
			for (gx = 0; gx < f->w; gx++)
				if (g[gx >> 3] & (0x80 >> (gx & 7)))
					canvas_fill(c, x + gx * scale, y + gy * scale,
					            scale, scale, color);
		x += f->w * scale;
	}
	return x;
}

/* Centered on the canvas width. */
static void draw_centered(struct canvas *c, const struct font *f, int y,
                          int scale, const char *s, uint32_t color)
{
	int w = utf8_count(s, (int)strlen(s)) * f->w * scale;

	draw_scaled(c, f, (c->w - w) / 2, y, scale, s, color);
}

/* Background and the title row with the log clock. */
static struct canvas *begin(uint32_t bg, const char *title)
{
	struct canvas *c = screen_canvas();
	char clock[32];

	canvas_fill(c, 0, 0, c->w, c->h, bg);
	text_draw(c, &font_big, MARGIN, 8, title, -1, C_TITLE);
	snprintf(clock, sizeof(clock), "t = %u ms", log_ms());
	text_draw(c, &font_big, c->w - MARGIN - (int)strlen(clock) * font_big.w, 8,
	          clock, -1, C_TEXT);
	return c;
}

static unsigned finish(void)
{
	unsigned t0 = log_ms();

	screen_present();
	return log_ms() - t0;
}

int ui_open(void)
{
	pthread_mutex_lock(&ui_lock);
	ui_ok = screen_open() == 0;
	if (!ui_ok) {
		screen_close();
		log_msg("ui: no screen, running without one");
	}
	pthread_mutex_unlock(&ui_lock);
	return ui_ok ? 0 : -1;
}

void ui_close(void)
{
	pthread_mutex_lock(&ui_lock);
	if (ui_ok)
		screen_close();
	ui_ok = 0;
	pthread_mutex_unlock(&ui_lock);
}

unsigned ui_lead(int ngroups, unsigned ms)
{
	struct canvas *c;
	char line[64];
	unsigned rc = 0;

	pthread_mutex_lock(&ui_lock);
	if (ui_ok) {
		c = begin(C_GAP, "hello5  sound check");
		snprintf(line, sizeof(line), "chimes start in %u s", ms / 1000);
		draw_centered(c, &font_big, 120, 2, line, C_TEXT);
		snprintf(line, sizeof(line), "%d groups: group N = N chimes", ngroups);
		draw_centered(c, &font_big, 230, 1, line, C_TEXT);
		draw_centered(c, &font_big, 280, 1,
		              "count the chimes you hear in each group", C_DIM);
		rc = finish();
	}
	pthread_mutex_unlock(&ui_lock);
	return rc;
}

unsigned ui_chime(int group, int ngroups, const char *label, int chime, int playing)
{
	struct canvas *c;
	char line[64];
	unsigned rc = 0;
	int i, x, box = 40, pitch = 56;

	pthread_mutex_lock(&ui_lock);
	if (ui_ok) {
		snprintf(line, sizeof(line), "group %d of %d  -  %s", group, ngroups, label);
		c = begin(group_bg[(group - 1) % 6], line);

		snprintf(line, sizeof(line), "GROUP %d", group);
		draw_centered(c, &font_big, 56, 4, line, C_TEXT);
		snprintf(line, sizeof(line), "chime %d of %d", chime, group);
		draw_centered(c, &font_big, 196, 2, line, C_TEXT);

		/* one square per chime of the group: filled = played or playing */
		x = (c->w - (group * pitch - (pitch - box))) / 2;
		for (i = 1; i <= group; i++, x += pitch) {
			canvas_fill(c, x, 276, box, box, C_TEXT);
			if (i > chime || (i == chime && !playing))
				canvas_fill(c, x + 4, 280, box - 8, box - 8,
				            group_bg[(group - 1) % 6]);
			if (i == chime && !playing)   /* just ended: half filled */
				canvas_fill(c, x + 4, 280 + (box - 8) / 2, box - 8,
				            (box - 8) / 2, C_TEXT);
		}

		if (playing) {
			canvas_fill(c, MARGIN, 344, c->w - 2 * MARGIN, 112, C_BOX);
			draw_centered(c, &font_big, 368, 2, "PLAYING", C_BOXTXT);
		} else {
			draw_centered(c, &font_big, 368, 2, "quiet", C_DIM);
		}
		rc = finish();
	}
	pthread_mutex_unlock(&ui_lock);
	return rc;
}

unsigned ui_gap(int done, int next, const char *next_label, const char *note)
{
	struct canvas *c;
	char line[64];
	unsigned rc = 0;

	pthread_mutex_lock(&ui_lock);
	if (ui_ok) {
		c = begin(C_GAP, "hello5  sound check");
		snprintf(line, sizeof(line), "group %d done", done);
		draw_centered(c, &font_big, 100, 3, line, C_TEXT);
		if (next > 0)
			snprintf(line, sizeof(line), "next: group %d (%s)", next, next_label);
		else
			snprintf(line, sizeof(line), "that was the last group");
		draw_centered(c, &font_big, 240, 1, line, C_DIM);
		if (note != NULL)
			draw_centered(c, &font_big, 300, 1, note, C_TITLE);
		rc = finish();
	}
	pthread_mutex_unlock(&ui_lock);
	return rc;
}

struct wrap_ctx {
	struct canvas *c;
	int            y;
	uint32_t       color;
};

static void wrap_line(const char *line, int nbytes, void *arg)
{
	struct wrap_ctx *w = arg;

	text_draw(w->c, &font_big, MARGIN, w->y, line, nbytes, w->color);
	w->y += font_big.h + 2;
}

unsigned ui_step(int step, int nsteps, const char *title, const char *text,
                 const char *question, const char *big, int playing,
                 const char *note)
{
	struct canvas *c;
	struct wrap_ctx w;
	char line[80];
	unsigned rc = 0;
	int cols;

	pthread_mutex_lock(&ui_lock);
	if (ui_ok) {
		snprintf(line, sizeof(line), "step %d of %d - %s", step, nsteps, title);
		c = begin(group_bg[(step - 1) % 6], line);
		cols = (c->w - 2 * MARGIN) / font_big.w;
		w.c = c;
		w.y = font_big.h + 20;
		w.color = C_TEXT;
		text_wrap(text, cols, wrap_line, &w);
		if (question != NULL) {
			w.y += 12;
			w.color = C_TITLE;
			text_wrap(question, cols, wrap_line, &w);
		}
		if (playing) {
			canvas_fill(c, MARGIN, 300, c->w - 2 * MARGIN, 104, C_BOX);
			draw_centered(c, &font_big, 320, 2, big, C_BOXTXT);
		} else {
			draw_centered(c, &font_big, 320, 2, big, C_TEXT);
		}
		if (note != NULL)
			draw_centered(c, &font_big, 424, 1, note, C_TEXT);
		rc = finish();
	}
	pthread_mutex_unlock(&ui_lock);
	return rc;
}

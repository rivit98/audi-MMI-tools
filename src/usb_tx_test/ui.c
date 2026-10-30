/*
 * ui.c - the usb_tx_test screen (see ui.h). Three layouts:
 *
 *   no picture  status bar coloured by link state, its detail line, what
 *               to do next on the phone (large, word-wrapped), and a
 *               footer: the status panel, full width.
 *   landscape   the picture centred on the whole screen; a slim status bar
 *   picture     (state, detail, video stats) over its top only when the
 *               phone asks for details (VIDEO_DETAILS). A 19.5:9 phone
 *               (800x368) leaves 56 px above the picture, so the bar
 *               covers none of it.
 *   portrait    the picture on the left, full height; the status panel in
 *   picture     the rest of the width, with the video stats as its body.
 *
 * (Text messages from the phone, shown here until 2026-10-25, are gone:
 * the phone's screen is what the app is for.)
 */
#include <stdio.h>
#include <string.h>

#include "font_big.h"
#include "font_small.h"
#include "log.h"
#include "model.h"
#include "ui.h"
#include "video.h"

#define STATUS_H     48
#define DETAIL_Y     54
#define BODY_TOP     80
#define FOOTER_H     24
#define MARGIN       12
#define MAX_LINES    16

#define C_BG        0xFF101820u
#define C_TEXT      0xFFE8E8E8u
#define C_DIM       0xFF8A929Eu
#define C_FOOTER_BG 0xFF1C2836u
#define C_RULE      0xFF2A3644u
#define C_VIDEO_BG  0xFF000000u

static uint32_t state_colour(enum link_state s)
{
	switch (s) {
	case LINK_NO_STACK:  return 0xFFA03028u;
	case LINK_WAITING:   return 0xFF3A4452u;
	case LINK_SWITCHING: return 0xFFB07A10u;
	case LINK_ACCESSORY: return 0xFF2F6FA8u;
	case LINK_CONNECTED: return 0xFF2E8E4Au;
	}
	return 0xFF3A4452u;
}

static const char *state_title(enum link_state s)
{
	switch (s) {
	case LINK_NO_STACK:  return "USB stack not ready";
	case LINK_WAITING:   return "Waiting for phone";
	case LINK_SWITCHING: return "Phone found, switching...";
	case LINK_ACCESSORY: return "Phone connected, app not open";
	case LINK_CONNECTED: return "Connected";
	}
	return "?";
}

/* What to do next, for the screen without a picture. */
static const char *state_hint(enum link_state s)
{
	switch (s) {
	case LINK_NO_STACK:
		return "Waiting for the MMI's USB stack. If this stays, pull the "
		       "SD card and try again after a restart.";
	case LINK_WAITING:
		return "Plug the phone into the car's USB port.";
	case LINK_SWITCHING:
		return "The phone is switching to accessory mode. If it asks, let "
		       "usb_tx_test open.";
	case LINK_ACCESSORY:
		return "Open usb_tx_test on the phone.";
	case LINK_CONNECTED:
		return "Tap \"Stream screen\" on the phone and allow the screen "
		       "capture. The phone's screen appears here.";
	}
	return "";
}

/* --- word wrap -------------------------------------------------------- */

struct wrapped {
	int n;
	const char *line[MAX_LINES];
	int len[MAX_LINES];
};

static void collect_line(const char *line, int nbytes, void *arg)
{
	struct wrapped *w = arg;

	if (w->n < MAX_LINES) {
		w->line[w->n] = line;
		w->len[w->n] = nbytes;
		w->n++;
	}
}

/* Word-wrapped text from y down, in [x0, x0 + pw); stops above y_max. */
static void draw_wrapped(struct canvas *c, const struct font *f, int x0, int pw,
                         int y, int y_max, const char *s, uint32_t colour)
{
	struct wrapped w;
	int i;

	memset(&w, 0, sizeof(w));
	text_wrap(s, (pw - 2 * MARGIN) / f->w, collect_line, &w);
	for (i = 0; i < w.n && y + f->h <= y_max; i++, y += f->h)
		text_draw(c, f, x0 + MARGIN, y, w.line[i], w.len[i], colour);
}

static void video_stats(char *out, size_t cap, const struct video_view *v)
{
	snprintf(out, cap, "video %u.%u fps  %dx%d  %u KB  dec %u ms  out %u ms",
	         v->fps_x10 / 10, v->fps_x10 % 10, v->frame.w, v->frame.h,
	         (v->bytes + 512) / 1024, v->decode_ms, v->present_ms);
}

/* The video stats one per line, for the portrait panel. */
static void draw_video_lines(struct canvas *c, int x0, int y, const struct video_view *v)
{
	char line[80];

	snprintf(line, sizeof(line), "picture %dx%d, %u KB", v->frame.w, v->frame.h,
	         (v->bytes + 512) / 1024);
	text_draw(c, &font_small, x0 + MARGIN, y, line, -1, C_TEXT);
	y += font_small.h + 4;
	snprintf(line, sizeof(line), "%u.%u pictures a second", v->fps_x10 / 10,
	         v->fps_x10 % 10);
	text_draw(c, &font_small, x0 + MARGIN, y, line, -1, C_TEXT);
	y += font_small.h + 4;
	snprintf(line, sizeof(line), "decode %u ms, on screen %u ms", v->decode_ms,
	         v->present_ms);
	text_draw(c, &font_small, x0 + MARGIN, y, line, -1, C_TEXT);
	y += font_small.h + 4;
	snprintf(line, sizeof(line), "shown %u, bad %u, dropped %u", v->shown, v->bad,
	         v->dropped);
	text_draw(c, &font_small, x0 + MARGIN, y, line, -1, C_DIM);
}

/* Copy the picture with its top-left at x,y; clipped to the canvas. */
static void blit(struct canvas *c, const struct canvas *src, int x, int y)
{
	int row, x0 = x < 0 ? -x : 0, w = src->w - x0;

	if (x + x0 + w > c->w)
		w = c->w - x - x0;
	if (w <= 0)
		return;
	for (row = 0; row < src->h; row++) {
		if (y + row < 0 || y + row >= c->h)
			continue;
		memcpy(c->px + (y + row) * c->stride + x + x0,
		       src->px + row * src->stride + x0, (size_t)w * 4);
	}
}

/* The status panel in [x0, x0 + pw): status bar, detail, body, footer.
 * The body is the video stats with a picture, else what to do next. */
static void draw_panel(struct canvas *c, struct model *m, int x0, int pw,
                       const char *sd_path, const struct video_view *v)
{
	char line[160];
	unsigned up = log_ms() / 1000;

	canvas_fill(c, x0, 0, pw, STATUS_H, state_colour(m->state));
	text_draw(c, &font_big, x0 + MARGIN, (STATUS_H - font_big.h) / 2,
	          state_title(m->state), -1, C_TEXT);
	text_draw(c, &font_small, x0 + MARGIN, DETAIL_Y, m->detail, -1, C_DIM);
	canvas_fill(c, x0, BODY_TOP - 4, pw, 1, C_RULE);

	if (v != NULL)
		draw_video_lines(c, x0, BODY_TOP + 8, v);
	else
		draw_wrapped(c, &font_big, x0, pw, BODY_TOP + 24, c->h - FOOTER_H - 8,
		             state_hint(m->state), C_TEXT);

	/* footer */
	canvas_fill(c, x0, c->h - FOOTER_H, pw, FOOTER_H, C_FOOTER_BG);
	if (v != NULL)
		snprintf(line, sizeof(line), "up %02u:%02u:%02u  pull SD to exit",
		         up / 3600, up / 60 % 60, up % 60);
	else
		snprintf(line, sizeof(line), "usb_tx_test  up %02u:%02u:%02u  log "
		         "%s/usb_tx_test.log  pull SD to exit", up / 3600,
		         up / 60 % 60, up % 60, sd_path);
	text_draw(c, &font_small, x0 + MARGIN,
	          c->h - FOOTER_H + (FOOTER_H - font_small.h) / 2, line, -1, C_DIM);
}

static void render_landscape(struct canvas *c, struct model *m,
                             const struct video_view *v)
{
	char line[160];
	int x;

	canvas_fill(c, 0, 0, c->w, c->h, C_VIDEO_BG);
	blit(c, &v->frame, (c->w - v->frame.w) / 2, (c->h - v->frame.h) / 2);
	if (!m->details)
		return;

	canvas_fill(c, 0, 0, c->w, VIDEO_TOPBAR_H, state_colour(m->state));
	snprintf(line, sizeof(line), "%s  %s", state_title(m->state), m->detail);
	text_draw(c, &font_small, MARGIN, (VIDEO_TOPBAR_H - font_small.h) / 2, line, -1, C_TEXT);
	video_stats(line, sizeof(line), v);
	x = c->w - MARGIN - utf8_count(line, (int)strlen(line)) * font_small.w;
	canvas_fill(c, x - MARGIN, 0, c->w - x + MARGIN, VIDEO_TOPBAR_H, state_colour(m->state));
	text_draw(c, &font_small, x, (VIDEO_TOPBAR_H - font_small.h) / 2, line, -1, C_TEXT);
}

static void render_portrait(struct canvas *c, struct model *m, const char *sd_path,
                            const struct video_view *v)
{
	int col = v->frame.w;

	canvas_fill(c, 0, 0, col, c->h, C_VIDEO_BG);
	blit(c, &v->frame, 0, (c->h - v->frame.h) / 2);
	canvas_fill(c, col, 0, 1, c->h, C_RULE);
	draw_panel(c, m, col + 1, c->w - col - 1, sd_path, v);
}

void ui_render(struct canvas *c, const char *sd_path)
{
	const struct video_view *v = video_view();
	struct model *m;

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	m = model_lock();
	if (v == NULL)
		draw_panel(c, m, 0, c->w, sd_path, NULL);
	else if (v->portrait)
		render_portrait(c, m, sd_path, v);
	else
		render_landscape(c, m, v);
	model_unlock();
}

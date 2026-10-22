/*
 * ui.c - the usb_tx_test screen (see ui.h). Three layouts:
 *
 *   text only   status bar coloured by link state, its detail line, the
 *               received messages newest at the bottom (word-wrapped), and
 *               a footer: the text panel, full width.
 *   landscape   a slim status bar (state, detail, video stats), the picture
 *   picture     centred below it; the newest message as a caption over the
 *               picture's bottom for CAPTION_MS after it arrived.
 *   portrait    the picture on the left, full height; the text panel in
 *   picture     the rest of the width.
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
#define MSGS_TOP     80
#define FOOTER_H     24
#define MSG_GAP      8
#define MARGIN       12
#define MAX_LINES    64
#define MSG_LINES    5     /* on screen per message; the log has it all */
#define CAPTION_MS   15000
#define CAPTION_LINES 2

#define C_BG        0xFF101820u
#define C_TEXT      0xFFE8E8E8u
#define C_DIM       0xFF8A929Eu
#define C_SEQ       0xFF5AC8FAu
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

/* --- word-wrapped message, bottom-up ---------------------------------- */

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
	}
	w->n++;
}

/* Draws one message ending at y_bottom; returns its top y. A message over
 * MSG_LINES shows its first MSG_LINES-1 lines and a "+N more" line. Lines
 * above y_min are left out. */
static int draw_message(struct canvas *c, const struct model_msg *msg,
                        int x0, int pw, int y_bottom, int y_min)
{
	const int cols = (pw - 2 * MARGIN) / font_big.w;
	struct wrapped w;
	char head[48], more[64];
	int i, y, n;

	memset(&w, 0, sizeof(w));
	text_wrap(msg->text, cols, collect_line, &w);
	n = w.n < MAX_LINES ? w.n : MAX_LINES;

	y = y_bottom;
	if (w.n > MSG_LINES) {
		n = MSG_LINES - 1;
		snprintf(more, sizeof(more), "... +%d more lines (full text in the log)",
		         w.n - n);
		y -= font_big.h;
		if (y < y_min)
			return y_min;
		text_draw(c, &font_small, x0 + MARGIN, y + (font_big.h - font_small.h) / 2,
		          more, -1, C_DIM);
	}
	for (i = n - 1; i >= 0; i--) {
		y -= font_big.h;
		if (y < y_min)
			return y_min;
		text_draw(c, &font_big, x0 + MARGIN, y, w.line[i], w.len[i], C_TEXT);
	}
	y -= font_small.h + 2;
	if (y < y_min)
		return y_min;
	snprintf(head, sizeof(head), "#%u  +%02u:%02u", msg->seq,
	         msg->ms / 60000, msg->ms / 1000 % 60);
	text_draw(c, &font_small, x0 + MARGIN, y, head, -1, C_SEQ);
	return y;
}

static void video_stats(char *out, size_t cap, const struct video_view *v)
{
	snprintf(out, cap, "video %u.%u fps  %dx%d  %u KB  dec %u ms  out %u ms",
	         v->fps_x10 / 10, v->fps_x10 % 10, v->frame.w, v->frame.h,
	         (v->bytes + 512) / 1024, v->decode_ms, v->present_ms);
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

/* The text panel in [x0, x0 + pw): status bar, detail, messages, footer. */
static void draw_panel(struct canvas *c, struct model *m, int x0, int pw,
                       const char *sd_path, const struct video_view *v)
{
	char line[160];
	unsigned up = log_ms() / 1000;
	int y, i;

	canvas_fill(c, x0, 0, pw, STATUS_H, state_colour(m->state));
	text_draw(c, &font_big, x0 + MARGIN, (STATUS_H - font_big.h) / 2,
	          state_title(m->state), -1, C_TEXT);
	text_draw(c, &font_small, x0 + MARGIN, DETAIL_Y, m->detail, -1, C_DIM);
	canvas_fill(c, x0, MSGS_TOP - 4, pw, 1, C_RULE);

	/* messages, newest at the bottom */
	y = c->h - FOOTER_H - MSG_GAP;
	if (m->nmsgs == 0)
		text_draw(c, &font_big, x0 + MARGIN, MSGS_TOP + 8,
		          "No messages yet.", -1, C_DIM);
	for (i = (int)m->nmsgs - 1; i >= 0 && y > MSGS_TOP; i--) {
		y = draw_message(c, model_msg_at(m, (unsigned)i), x0, pw, y, MSGS_TOP);
		y -= MSG_GAP;
	}

	/* footer */
	canvas_fill(c, x0, c->h - FOOTER_H, pw, FOOTER_H, C_FOOTER_BG);
	if (v != NULL)
		snprintf(line, sizeof(line), "rx %u  %u.%u fps  dec %u ms  out %u ms",
		         m->rx_count, v->fps_x10 / 10, v->fps_x10 % 10, v->decode_ms,
		         v->present_ms);
	else
		snprintf(line, sizeof(line), "usb_tx_test  rx %u  up %02u:%02u:%02u  log "
		         "%s/usb_tx_test.log  pull SD to exit", m->rx_count, up / 3600,
		         up / 60 % 60, up % 60, sd_path);
	text_draw(c, &font_small, x0 + MARGIN,
	          c->h - FOOTER_H + (FOOTER_H - font_small.h) / 2, line, -1, C_DIM);
}

/* The newest message over the bottom of the picture area, if it is new. */
static void draw_caption(struct canvas *c, struct model *m, int x0, int pw)
{
	const int cols = (pw - 2 * MARGIN) / font_big.w;
	const struct model_msg *msg;
	struct wrapped w;
	char head[32];
	int i, n, band, y;

	if (m->nmsgs == 0)
		return;
	msg = model_msg_at(m, m->nmsgs - 1);
	if (log_ms() - msg->ms > CAPTION_MS)
		return;
	memset(&w, 0, sizeof(w));
	text_wrap(msg->text, cols, collect_line, &w);
	n = w.n < CAPTION_LINES ? w.n : CAPTION_LINES;
	band = font_small.h + 4 + n * font_big.h + 6;
	y = c->h - band;
	canvas_fill(c, x0, y, pw, band, C_FOOTER_BG);
	snprintf(head, sizeof(head), "#%u%s", msg->seq, w.n > n ? "  (more in the list)" : "");
	text_draw(c, &font_small, x0 + MARGIN, y + 2, head, -1, C_SEQ);
	y += font_small.h + 4;
	for (i = 0; i < n; i++, y += font_big.h)
		text_draw(c, &font_big, x0 + MARGIN, y, w.line[i], w.len[i], C_TEXT);
}

static void render_landscape(struct canvas *c, struct model *m,
                             const struct video_view *v)
{
	char line[160];
	int area_h = c->h - VIDEO_TOPBAR_H, x;

	canvas_fill(c, 0, 0, c->w, VIDEO_TOPBAR_H, state_colour(m->state));
	snprintf(line, sizeof(line), "%s  %s", state_title(m->state), m->detail);
	text_draw(c, &font_small, MARGIN, (VIDEO_TOPBAR_H - font_small.h) / 2, line, -1, C_TEXT);
	video_stats(line, sizeof(line), v);
	x = c->w - MARGIN - utf8_count(line, (int)strlen(line)) * font_small.w;
	canvas_fill(c, x - MARGIN, 0, c->w - x + MARGIN, VIDEO_TOPBAR_H, state_colour(m->state));
	text_draw(c, &font_small, x, (VIDEO_TOPBAR_H - font_small.h) / 2, line, -1, C_TEXT);

	canvas_fill(c, 0, VIDEO_TOPBAR_H, c->w, area_h, C_VIDEO_BG);
	blit(c, &v->frame, (c->w - v->frame.w) / 2,
	     VIDEO_TOPBAR_H + (area_h - v->frame.h) / 2);
	draw_caption(c, m, 0, c->w);
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

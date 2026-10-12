/*
 * ui.c - the usb_tx_test screen (see ui.h): status bar coloured by link
 * state, its detail line, the received messages newest at the bottom
 * (word-wrapped, a long one shows its end), and a footer.
 */
#include <stdio.h>
#include <string.h>

#include "font_big.h"
#include "font_small.h"
#include "log.h"
#include "model.h"
#include "ui.h"

#define STATUS_H     48
#define DETAIL_Y     54
#define MSGS_TOP     80
#define FOOTER_H     24
#define MSG_GAP      8
#define MARGIN       12
#define MAX_LINES    64
#define MSG_LINES    5     /* on screen per message; the log has it all */

#define C_BG        0xFF101820u
#define C_TEXT      0xFFE8E8E8u
#define C_DIM       0xFF8A929Eu
#define C_SEQ       0xFF5AC8FAu
#define C_FOOTER_BG 0xFF1C2836u
#define C_RULE      0xFF2A3644u

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
                        int y_bottom, int y_min)
{
	const int cols = (c->w - 2 * MARGIN) / font_big.w;
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
		text_draw(c, &font_small, MARGIN, y + (font_big.h - font_small.h) / 2,
		          more, -1, C_DIM);
	}
	for (i = n - 1; i >= 0; i--) {
		y -= font_big.h;
		if (y < y_min)
			return y_min;
		text_draw(c, &font_big, MARGIN, y, w.line[i], w.len[i], C_TEXT);
	}
	y -= font_small.h + 2;
	if (y < y_min)
		return y_min;
	snprintf(head, sizeof(head), "#%u  +%02u:%02u", msg->seq,
	         msg->ms / 60000, msg->ms / 1000 % 60);
	text_draw(c, &font_small, MARGIN, y, head, -1, C_SEQ);
	return y;
}

void ui_render(struct canvas *c, const char *sd_path)
{
	struct model *m;
	char line[160];
	unsigned up = log_ms() / 1000;
	int y, i;

	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	m = model_lock();

	/* status bar */
	canvas_fill(c, 0, 0, c->w, STATUS_H, state_colour(m->state));
	text_draw(c, &font_big, MARGIN, (STATUS_H - font_big.h) / 2,
	          state_title(m->state), -1, C_TEXT);
	text_draw(c, &font_small, MARGIN, DETAIL_Y, m->detail, -1, C_DIM);
	canvas_fill(c, 0, MSGS_TOP - 4, c->w, 1, C_RULE);

	/* messages, newest at the bottom */
	y = c->h - FOOTER_H - MSG_GAP;
	if (m->nmsgs == 0)
		text_draw(c, &font_big, MARGIN, MSGS_TOP + 8,
		          "No messages yet.", -1, C_DIM);
	for (i = (int)m->nmsgs - 1; i >= 0 && y > MSGS_TOP; i--) {
		y = draw_message(c, model_msg_at(m, (unsigned)i), y, MSGS_TOP);
		y -= MSG_GAP;
	}

	/* footer */
	canvas_fill(c, 0, c->h - FOOTER_H, c->w, FOOTER_H, C_FOOTER_BG);
	snprintf(line, sizeof(line), "usb_tx_test  rx %u  up %02u:%02u:%02u  log %s/usb_tx_test.log"
	         "  pull SD to exit", m->rx_count, up / 3600, up / 60 % 60, up % 60, sd_path);
	model_unlock();
	text_draw(c, &font_small, MARGIN, c->h - FOOTER_H + (FOOTER_H - font_small.h) / 2,
	          line, -1, C_DIM);
}


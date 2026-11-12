// ui.cpp - see ui.h.
#include "ui.h"

extern "C" {
#include "font_big.h"
#include "font_small.h"
#include "screen.h"
#include "text.h"
}

namespace ui {

namespace {

constexpr int kMargin = 20;
constexpr uint32_t C_BG = 0xff101828u;
constexpr uint32_t C_TITLE = 0xffffd040u;
constexpr uint32_t C_TEXT = 0xffffffffu;
constexpr uint32_t C_FOOT = 0xff90a0b0u;
constexpr uint32_t C_BAR = 0xff2a3a58u;

bool is_open = false;

struct Wrap {
	struct canvas *c;
	int y;
};

void draw_line(const char *line, int nbytes, void *arg)
{
	Wrap *w = static_cast<Wrap *>(arg);
	if (w->y + font_big.h <= w->c->h - 2 * font_small.h)
		text_draw(w->c, &font_big, kMargin, w->y, line, nbytes, C_TEXT);
	w->y += font_big.h + 4;
}

struct canvas *begin(const char *title)
{
	if (!is_open) {
		if (screen_open() != 0) {
			screen_close();
			return nullptr;
		}
		is_open = true;
	}
	struct canvas *c = screen_canvas();
	canvas_fill(c, 0, 0, c->w, c->h, C_BG);
	canvas_fill(c, 0, 0, c->w, font_big.h + 16, C_BAR);
	text_draw(c, &font_big, kMargin, 8, title, -1, C_TITLE);
	return c;
}

} // namespace

int show(const char *title, const std::string &body, const char *footer)
{
	struct canvas *c = begin(title);
	if (c == nullptr)
		return -1;
	Wrap w{c, font_big.h + 32};
	text_wrap(body.c_str(), (c->w - 2 * kMargin) / font_big.w, draw_line, &w);
	if (footer != nullptr)
		text_draw(c, &font_small, kMargin, c->h - font_small.h - 8, footer, -1, C_FOOT);
	return screen_present();
}

int show_list(const char *title, const std::vector<std::string> &lines)
{
	struct canvas *c = begin(title);
	if (c == nullptr)
		return (int)lines.size();
	int y = font_big.h + 24;
	size_t i = 0;
	for (; i < lines.size() && y + font_small.h <= c->h; i++, y += font_small.h + 1)
		text_draw(c, &font_small, kMargin, y, lines[i].c_str(), -1, C_TEXT);
	screen_present();
	return (int)(lines.size() - i);
}

void hide()
{
	if (is_open)
		screen_close();
	is_open = false;
}

} // namespace ui

// ui.cpp - the audi_stream_receiver screen (see ui.h): a status bar
// coloured by link state, its detail line, what to do next on the phone
// (large, word-wrapped), and a footer. While the H.264 stream plays the
// screen is the video's (main.cpp) and this is not shown.
//
// (There are no text messages from the phone: the phone's screen is what
// the app is for.)
#include "ui.h"
#include "types.h"

#include <cstdio>

extern "C" {
#include "cpumon.h"
#include "font_big.h"
#include "font_small.h"
#include "log.h"
}

#include "model.h"

using model::LinkState;

namespace ui {

namespace {

constexpr int kStatusH = 48;
constexpr int kDetailY = 54;
constexpr int kBodyTop = 80;
constexpr int kFooterH = 24;
constexpr int kMargin = 12;
constexpr int kMaxLines = 16;

constexpr u32 kBg = 0xFF101820u;
constexpr u32 kText = 0xFFE8E8E8u;
constexpr u32 kDim = 0xFF8A929Eu;
constexpr u32 kFooterBg = 0xFF1C2836u;
constexpr u32 kRule = 0xFF2A3644u;

u32 state_colour(LinkState s)
{
	switch (s) {
	case LinkState::NoStack:   return 0xFFA03028u;
	case LinkState::Waiting:   return 0xFF3A4452u;
	case LinkState::Switching: return 0xFFB07A10u;
	case LinkState::Accessory: return 0xFF2F6FA8u;
	case LinkState::Connected: return 0xFF2E8E4Au;
	}
	return 0xFF3A4452u;
}

const char *state_title(LinkState s)
{
	switch (s) {
	case LinkState::NoStack:   return "USB stack not ready";
	case LinkState::Waiting:   return "Waiting for phone";
	case LinkState::Switching: return "Phone found, switching...";
	case LinkState::Accessory: return "Phone connected, app not open";
	case LinkState::Connected: return "Connected";
	}
	return "?";
}

// What to do next.
const char *state_hint(LinkState s)
{
	switch (s) {
	case LinkState::NoStack:
		return "Waiting for the MMI's USB stack. If this stays, pull the "
		       "SD card and try again after a restart.";
	case LinkState::Waiting:
		return "Plug the phone into the car's USB port.";
	case LinkState::Switching:
		return "The phone is switching to accessory mode. If it asks, let "
		       "Audi streaming open.";
	case LinkState::Accessory:
		return "Open Audi streaming on the phone.";
	case LinkState::Connected:
		return "Start Android Auto's head unit server on the phone (once: "
		       "Android Auto's developer menu). Android Auto appears here.";
	}
	return "";
}

// --- word wrap ----------------------------------------------------------

struct Wrapped {
	int n = 0;
	const char *line[kMaxLines];
	int len[kMaxLines];
};

// Word-wrapped text from y down, in [x0, x0 + pw); stops above y_max.
void draw_wrapped(canvas *c, const font *f, int x0, int pw, int y, int y_max, const char *s,
                  u32 colour)
{
	Wrapped w;

	text_wrap(s, (pw - 2 * kMargin) / f->w,
	          [](const char *line, int nbytes, void *arg) {
		          Wrapped *out = static_cast<Wrapped *>(arg);
		          if (out->n < kMaxLines) {
			          out->line[out->n] = line;
			          out->len[out->n] = nbytes;
			          out->n++;
		          }
	          },
	          &w);
	for (int i = 0; i < w.n && y + f->h <= y_max; i++, y += f->h)
		text_draw(c, f, x0 + kMargin, y, w.line[i], w.len[i], colour);
}

// "cpu 27% unit 55%" (cpumon.h; " HIGH" when the last window was), the
// last second.
void cpu_text(char *out, usize cap)
{
	int self, unit, high;

	cpumon_now(&self, &unit, &high);
	if (self < 0)
		std::snprintf(out, cap, "cpu ?");
	else if (unit < 0)
		std::snprintf(out, cap, "cpu %d%%%s", self, high ? " HIGH" : "");
	else
		std::snprintf(out, cap, "cpu %d%% unit %d%%%s", self, unit, high ? " HIGH" : "");
}

// The status panel in [x0, x0 + pw): status bar, detail, what to do next,
// footer.
void draw_panel(canvas *c, const model::Model &m, int x0, int pw, const char *out_dir)
{
	char line[160], cpu[48];
	unsigned up = log_ms() / 1000;

	canvas_fill(c, x0, 0, pw, kStatusH, state_colour(m.state));
	text_draw(c, &font_big, x0 + kMargin, (kStatusH - font_big.h) / 2,
	          state_title(m.state), -1, kText);
	text_draw(c, &font_small, x0 + kMargin, kDetailY, m.detail.c_str(), -1, kDim);
	canvas_fill(c, x0, kBodyTop - 4, pw, 1, kRule);

	draw_wrapped(c, &font_big, x0, pw, kBodyTop + 24, c->h - kFooterH - 8,
	             state_hint(m.state), kText);

	// footer
	canvas_fill(c, x0, c->h - kFooterH, pw, kFooterH, kFooterBg);
	cpu_text(cpu, sizeof(cpu));
	std::snprintf(line, sizeof(line), "up %02u:%02u:%02u  %s  log "
	              "%s/audi_stream_receiver.log  pull SD to exit", up / 3600,
	              up / 60 % 60, up % 60, cpu, out_dir);
	text_draw(c, &font_small, x0 + kMargin,
	          c->h - kFooterH + (kFooterH - font_small.h) / 2, line, -1, kDim);
}

} // namespace

void render(canvas *c, const char *out_dir)
{
	model::Model m = model::get();

	canvas_fill(c, 0, 0, c->w, c->h, kBg);
	draw_panel(c, m, 0, c->w, out_dir);
}

} // namespace ui

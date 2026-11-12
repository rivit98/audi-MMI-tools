// ui.cpp - the audi_stream_receiver screen (see ui.h). Three layouts:
//
//   no picture  status bar coloured by link state, its detail line, what
//               to do next on the phone (large, word-wrapped), and a
//               footer: the status panel, full width.
//   landscape   the picture centred on the whole screen; a slim status bar
//   picture     (state, detail, video stats) over its top only when the
//               phone asks for details (VIDEO_DETAILS). A 19.5:9 phone
//               (800x368) leaves 56 px above the picture, so the bar
//               covers none of it.
//   portrait    the picture on the left, full height; the status panel in
//   picture     the rest of the width, with the video stats as its body.
//
// (There are no text messages from the phone: the phone's screen is what
// the app is for.)
#include "ui.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "cpumon.h"
#include "font_big.h"
#include "font_small.h"
#include "log.h"
}

#include "model.h"
#include "video.h"

using model::LinkState;

namespace ui {

namespace {

constexpr int kStatusH = 48;
constexpr int kDetailY = 54;
constexpr int kBodyTop = 80;
constexpr int kFooterH = 24;
constexpr int kMargin = 12;
constexpr int kMaxLines = 16;

constexpr uint32_t kBg = 0xFF101820u;
constexpr uint32_t kText = 0xFFE8E8E8u;
constexpr uint32_t kDim = 0xFF8A929Eu;
constexpr uint32_t kFooterBg = 0xFF1C2836u;
constexpr uint32_t kRule = 0xFF2A3644u;
constexpr uint32_t kVideoBg = 0xFF000000u;

uint32_t state_colour(LinkState s)
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

// What to do next, for the screen without a picture.
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
		return "Tap \"Stream screen\" on the phone and allow the screen "
		       "capture. The phone's screen appears here.";
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
                  uint32_t colour)
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
void cpu_text(char *out, size_t cap)
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

void video_stats(char *out, size_t cap, const video::View *v)
{
	char cpu[48];

	cpu_text(cpu, sizeof(cpu));
	std::snprintf(out, cap, "video %u.%u fps  %dx%d  %u KB %s  dec %u ms  out %u ms  %s",
	              v->fps_x10 / 10, v->fps_x10 % 10, v->frame.w, v->frame.h,
	              (v->bytes + 512) / 1024, proto::fmt_name(v->format), v->decode_ms,
	              v->present_ms, cpu);
}

// The video stats one per line, for the portrait panel.
void draw_video_lines(canvas *c, int x0, int y, const video::View *v)
{
	char line[80];

	std::snprintf(line, sizeof(line), "picture %dx%d, %u KB %s", v->frame.w, v->frame.h,
	              (v->bytes + 512) / 1024, proto::fmt_name(v->format));
	text_draw(c, &font_small, x0 + kMargin, y, line, -1, kText);
	y += font_small.h + 4;
	std::snprintf(line, sizeof(line), "%u.%u pictures a second", v->fps_x10 / 10,
	              v->fps_x10 % 10);
	text_draw(c, &font_small, x0 + kMargin, y, line, -1, kText);
	y += font_small.h + 4;
	std::snprintf(line, sizeof(line), "decode %u ms, on screen %u ms", v->decode_ms,
	              v->present_ms);
	text_draw(c, &font_small, x0 + kMargin, y, line, -1, kText);
	y += font_small.h + 4;
	std::snprintf(line, sizeof(line), "shown %u, bad %u, dropped %u", v->shown, v->bad,
	              v->dropped);
	text_draw(c, &font_small, x0 + kMargin, y, line, -1, kDim);
}

// Copy the picture with its top-left at x,y; clipped to the canvas.
void blit(canvas *c, const canvas *src, int x, int y)
{
	int x0 = x < 0 ? -x : 0, w = src->w - x0;

	if (x + x0 + w > c->w)
		w = c->w - x - x0;
	if (w <= 0)
		return;
	for (int row = 0; row < src->h; row++) {
		if (y + row < 0 || y + row >= c->h)
			continue;
		std::memcpy(c->px + (y + row) * c->stride + x + x0,
		            src->px + row * src->stride + x0, (size_t)w * 4);
	}
}

// The status panel in [x0, x0 + pw): status bar, detail, body, footer.
// The body is the video stats with a picture, else what to do next.
void draw_panel(canvas *c, const model::Model &m, int x0, int pw, const char *out_dir,
                const video::View *v)
{
	char line[160], cpu[48];
	unsigned up = log_ms() / 1000;

	canvas_fill(c, x0, 0, pw, kStatusH, state_colour(m.state));
	text_draw(c, &font_big, x0 + kMargin, (kStatusH - font_big.h) / 2,
	          state_title(m.state), -1, kText);
	text_draw(c, &font_small, x0 + kMargin, kDetailY, m.detail.c_str(), -1, kDim);
	canvas_fill(c, x0, kBodyTop - 4, pw, 1, kRule);

	if (v != nullptr)
		draw_video_lines(c, x0, kBodyTop + 8, v);
	else
		draw_wrapped(c, &font_big, x0, pw, kBodyTop + 24, c->h - kFooterH - 8,
		             state_hint(m.state), kText);

	// footer
	canvas_fill(c, x0, c->h - kFooterH, pw, kFooterH, kFooterBg);
	cpu_text(cpu, sizeof(cpu));
	if (v != nullptr)
		std::snprintf(line, sizeof(line), "up %02u:%02u:%02u  %s  pull SD to exit",
		              up / 3600, up / 60 % 60, up % 60, cpu);
	else
		std::snprintf(line, sizeof(line), "up %02u:%02u:%02u  %s  log "
		              "%s/audi_stream_receiver.log  pull SD to exit", up / 3600,
		              up / 60 % 60, up % 60, cpu, out_dir);
	text_draw(c, &font_small, x0 + kMargin,
	          c->h - kFooterH + (kFooterH - font_small.h) / 2, line, -1, kDim);
}

void render_landscape(canvas *c, const model::Model &m, const video::View *v)
{
	char line[160];

	canvas_fill(c, 0, 0, c->w, c->h, kVideoBg);
	blit(c, &v->frame, (c->w - v->frame.w) / 2, (c->h - v->frame.h) / 2);
	if (!m.details)
		return;

	canvas_fill(c, 0, 0, c->w, video::kTopbarH, state_colour(m.state));
	std::snprintf(line, sizeof(line), "%s  %s", state_title(m.state), m.detail.c_str());
	text_draw(c, &font_small, kMargin, (video::kTopbarH - font_small.h) / 2, line, -1, kText);
	video_stats(line, sizeof(line), v);
	int x = c->w - kMargin - utf8_count(line, (int)std::strlen(line)) * font_small.w;
	canvas_fill(c, x - kMargin, 0, c->w - x + kMargin, video::kTopbarH, state_colour(m.state));
	text_draw(c, &font_small, x, (video::kTopbarH - font_small.h) / 2, line, -1, kText);
}

void render_portrait(canvas *c, const model::Model &m, const char *out_dir, const video::View *v)
{
	int col = v->frame.w;

	canvas_fill(c, 0, 0, col, c->h, kVideoBg);
	blit(c, &v->frame, 0, (c->h - v->frame.h) / 2);
	canvas_fill(c, col, 0, 1, c->h, kRule);
	draw_panel(c, m, col + 1, c->w - col - 1, out_dir, v);
}

} // namespace

void render(canvas *c, const char *out_dir)
{
	const video::View *v = video::view();
	model::Model m = model::get();

	canvas_fill(c, 0, 0, c->w, c->h, kBg);
	if (v == nullptr)
		draw_panel(c, m, 0, c->w, out_dir, nullptr);
	else if (v->portrait)
		render_portrait(c, m, out_dir, v);
	else
		render_landscape(c, m, v);
}

} // namespace ui

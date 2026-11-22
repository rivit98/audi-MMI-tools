// soft.cpp - hello10's sw test (soft.h): software H.264 decoding and the
// picture's way to the screen through the 2D engine (gf_draw_image).
#include "soft.h"

extern "C" {
#include "log.h"
#include "quit.h"
#include "lmgr.h"
}

#include "swdec.h"
#include "ui.h"

#define _Static_assert static_assert   // the SDK's gf.h checks its layouts the C way
extern "C" {
#include <gf/gf.h>
}
#undef _Static_assert

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

// libgf's 2D drawing calls; sdk/include/gf/gf.h declares only what the
// other apps use. Signatures as in QNX 6.3's GF API; all are exported by
// the unit's libgf.so.1 (stubbed by the SDK).
extern "C" {
typedef struct _gf_context *gf_context_t;
int gf_context_create(gf_context_t *pcontext);
void gf_context_free(gf_context_t context);
int gf_context_set_surface(gf_context_t context, gf_surface_t surface);
int gf_draw_begin(gf_context_t context);
void gf_draw_end(gf_context_t context);
int gf_draw_finish(gf_context_t context);
int gf_draw_image(gf_context_t context, const u8 *image, gf_format_t format, int stride,
                  const gf_palette_t *palette, int x, int y, int w, int h, unsigned flags);
}

namespace soft {
namespace {

constexpr int kCid = 40;            // the apps' displayable (screen.c's)
constexpr int kRegisterFlags = 2;   // showScreen's
constexpr unsigned kDecodeMs = 20000;
constexpr unsigned kBgraMs = 15000;
constexpr unsigned kYuvMs = 10000;
constexpr unsigned kWindowMs = 5000;
constexpr unsigned kLabelMs = 7000;
constexpr const char *kFooter = "hello10 - software decoder. Pull the SD card to stop.";

// Per-frame times in microseconds.
class Series {
public:
	void add(u32 us)
	{
		v_.push_back(us);
		sum_ += us;
	}
	usize n() const { return v_.size(); }
	f64 avg_ms() const { return v_.empty() ? 0 : (f64)sum_ / (f64)v_.size() / 1000.0; }
	// "min/median/p90/max" in ms
	std::string spread() const
	{
		if (v_.empty())
			return "-";
		std::vector<u32> s = v_;
		std::sort(s.begin(), s.end());
		char b[80];
		std::snprintf(b, sizeof(b), "%.1f/%.1f/%.1f/%.1f", s.front() / 1000.0,
		              s[s.size() / 2] / 1000.0, s[s.size() * 9 / 10] / 1000.0, s.back() / 1000.0);
		return b;
	}

private:
	std::vector<u32> v_;
	u64 sum_ = 0;
};

f64 fps(usize frames, unsigned ms)
{
	return ms ? (f64)frames * 1000.0 / (f64)ms : 0;
}

bool sleep_quit(unsigned ms)
{
	unsigned t0 = log_ms();
	while (quit_poll() == QUIT_NONE) {
		if (log_ms() - t0 >= ms)
			return false;
		usleep(50 * 1000);
	}
	return true;
}

// The clip, looped (frame 0 is an IDR, so looping restarts cleanly).
class Source {
public:
	Source(const u8 *data, const std::vector<h264::Range> &frames)
	    : data_(data), frames_(frames)
	{
	}
	~Source() { swdec_close(d_); }
	bool open()
	{
		d_ = swdec_open();
		return d_ != nullptr;
	}
	// Decodes the next frame; true with *pic if a picture came out.
	bool next(swdec_pic *pic, u32 &us)
	{
		const h264::Range &r = frames_[i_++ % frames_.size()];
		unsigned t0 = log_us();
		int rc = swdec_decode(d_, data_ + r.begin, r.end - r.begin, pic);
		us = log_us() - t0;
		if (rc < 0)
			errors_++;
		else if (rc == 1)
			concealed_ += pic->error_mbs != 0;
		return rc == 1;
	}
	u32 errors() const { return errors_; }
	u32 concealed() const { return concealed_; }

private:
	const u8 *data_;
	const std::vector<h264::Range> &frames_;
	swdec *d_ = nullptr;
	usize i_ = 0;
	u32 errors_ = 0, concealed_ = 0;
};

// Our displayable, drawn by the 2D engine. The VFB is double-buffered (its
// sid changes on every updateVfb), so up to two surfaces stay attached,
// each with its own context.
class GfOut {
public:
	~GfOut() { close(); }

	bool open()
	{
		gf_dev_info_t di;
		gf_display_info_t dpi;
		int rc = gf_dev_attach(&dev_, GF_DEVICE_INDEX(0), &di);
		log_msg("  gf_dev_attach: %d", rc);
		if (rc != GF_ERR_OK)
			return false;
		rc = gf_display_attach(&display_, dev_, 0, &dpi);
		log_msg("  gf_display_attach: %d %dx%d", rc, dpi.xres, dpi.yres);
		if (rc != GF_ERR_OK)
			return false;
		rc = lm_open();
		if (rc != 0) {
			log_msg("  lm_open: %s", std::strerror(rc));
			return false;
		}
		lm_open_ = true;
		lm_unregister_displayable(kCid);   // EINVAL = was not registered
		int result = 0;
		rc = lm_register_displayable(kCid, dpi.xres, dpi.yres, kRegisterFlags, &result);
		log_msg("  lm_register_displayable(%d, %dx%d): rc=%d result=%d", kCid, dpi.xres,
		        dpi.yres, rc, result);
		if (rc != 0 || result == 0)
			return false;
		registered_ = true;
		lm_vfb v;
		rc = lm_get_vfb(kCid, &v);
		log_msg("  lm_get_vfb(%d): rc=%d sid=%u", kCid, rc, v.sid);
		if (rc != 0)
			return false;
		sid_ = v.sid;
		int cids[3] = {kCid, 16, 19};
		rc = lm_show_displayables(0, cids, 3);
		log_msg("  lm_show_displayables(0, {%d,16,19}): rc=%d", kCid, rc);
		return rc == 0;
	}

	// One picture at (0,0). Returns the gf_draw_image result (0 = drawn).
	int draw(const u8 *img, gf_format_t fmt, int stride, int w, int h, u32 &draw_us, u32 &flip_us)
	{
		unsigned t0 = log_us();
		gf_context_t ctx = context_for(sid_);
		if (ctx == nullptr)
			return -1;
		int rc = gf_draw_begin(ctx);
		if (rc == GF_ERR_OK) {
			rc = gf_draw_image(ctx, img, fmt, stride, nullptr, 0, 0, w, h, 0);
			int fin = gf_draw_finish(ctx);
			gf_draw_end(ctx);
			if (rc == GF_ERR_OK)
				rc = fin;
		}
		unsigned t1 = log_us();
		draw_us = t1 - t0;
		if (rc != GF_ERR_OK)
			return rc;
		lm_vfb v;
		int lrc = lm_update_vfb(kCid);
		if (lrc == 0)
			lrc = lm_get_vfb(kCid, &v);
		flip_us = log_us() - t1;
		if (lrc != 0) {
			log_msg("  updateVfb/getVfb: %s", std::strerror(lrc));
			return -1;
		}
		sid_ = v.sid;
		return 0;
	}

	void close()
	{
		for (Slot &s : slots_) {
			if (s.ctx != nullptr)
				gf_context_free(s.ctx);
			if (s.surface != nullptr)
				gf_surface_free(s.surface);
			s = Slot();
		}
		if (registered_)
			log_msg("  lm_unregister_displayable(%d): rc=%d", kCid,
			        lm_unregister_displayable(kCid));
		registered_ = false;
		if (lm_open_)
			lm_close();
		lm_open_ = false;
		if (display_ != nullptr)
			gf_display_detach(display_);
		if (dev_ != nullptr)
			gf_dev_detach(dev_);
		display_ = nullptr;
		dev_ = nullptr;
	}

private:
	struct Slot {
		unsigned sid = 0;
		gf_surface_t surface = nullptr;
		gf_context_t ctx = nullptr;
		unsigned used = 0;
	};

	gf_context_t context_for(unsigned sid)
	{
		for (Slot &s : slots_)
			if (s.ctx != nullptr && s.sid == sid) {
				s.used = ++tick_;
				return s.ctx;
			}
		Slot &s = slots_[0].used <= slots_[1].used ? slots_[0] : slots_[1];
		if (s.ctx != nullptr)
			gf_context_free(s.ctx);
		if (s.surface != nullptr)
			gf_surface_free(s.surface);
		s = Slot();
		int rc = gf_surface_attach_by_sid(&s.surface, dev_, sid);
		gf_surface_info_t si;
		std::memset(&si, 0, sizeof(si));
		if (rc == GF_ERR_OK)
			gf_surface_get_info(s.surface, &si);
		int crc = rc == GF_ERR_OK ? gf_context_create(&s.ctx) : -1;
		int src = crc == GF_ERR_OK ? gf_context_set_surface(s.ctx, s.surface) : -1;
		log_msg("  sid %u: attach %d (%dx%d format %d stride %u), context %d, set_surface %d",
		        sid, rc, si.w, si.h, (int)si.format, si.stride, crc, src);
		if (rc != GF_ERR_OK || crc != GF_ERR_OK || src != GF_ERR_OK) {
			if (s.ctx != nullptr)
				gf_context_free(s.ctx);
			if (s.surface != nullptr)
				gf_surface_free(s.surface);
			s = Slot();
			return nullptr;
		}
		s.sid = sid;
		s.used = ++tick_;
		return s.ctx;
	}

	gf_dev_t dev_ = nullptr;
	gf_display_t display_ = nullptr;
	bool lm_open_ = false, registered_ = false;
	unsigned sid_ = 0, tick_ = 0;
	Slot slots_[2];
};

// I420 (coded size) -> UYVY bytes, the visible part.
void to_uyvy(const swdec_pic &p, u8 *out)
{
	const u8 *Y = p.i420, *U = Y + p.width * p.height, *V = U + p.width * p.height / 4;
	for (int y = 0; y < p.crop_h; y++) {
		int sy = p.crop_top + y;
		const u8 *yr = Y + sy * p.width + p.crop_left;
		const u8 *ur = U + (sy / 2) * (p.width / 2) + p.crop_left / 2;
		const u8 *vr = V + (sy / 2) * (p.width / 2) + p.crop_left / 2;
		u8 *o = out + (usize)y * p.crop_w * 2;
		for (int x = 0; x + 1 < p.crop_w; x += 2) {
			*o++ = ur[x / 2];
			*o++ = yr[x];
			*o++ = vr[x / 2];
			*o++ = yr[x + 1];
		}
	}
}

// decode: as fast as it goes, no picture.
bool step_decode(Source &src, std::vector<std::string> &summary)
{
	ui::show("Software decoder 1/3: decoding only",
	         "The MMI decodes the 800x480 test video in software for 20 s, as fast as it "
	         "can. Nothing moves on the screen. Nothing to do.",
	         kFooter);
	Series all, idr, p;
	swdec_pic pic;
	unsigned t0 = log_ms(), win = t0;
	usize win_n = 0;
	while (quit_poll() == QUIT_NONE && log_ms() - t0 < kDecodeMs) {
		u32 us;
		if (!src.next(&pic, us))
			continue;
		all.add(us);
		(pic.idr ? idr : p).add(us);
		win_n++;
		if (log_ms() - win >= kWindowMs) {
			log_msg("  sw decode t=%u s: %.1f fps", (log_ms() - t0) / 1000,
			        fps(win_n, log_ms() - win));
			win = log_ms();
			win_n = 0;
		}
	}
	unsigned ms = log_ms() - t0;
	ui::hide();
	char b[200];
	std::snprintf(b, sizeof(b), "sw decode: %.1f fps, %zu frames, ms %s (IDR %.1f, P %.1f)",
	              fps(all.n(), ms), all.n(), all.spread().c_str(), idr.avg_ms(), p.avg_ms());
	log_msg("RESULT sw_decode frames=%zu ms=%u fps=%.1f decode_ms(min/med/p90/max)=%s "
	        "idr=%zu idr_avg=%.1f p_avg=%.1f errors=%u concealed=%u",
	        all.n(), ms, fps(all.n(), ms), all.spread().c_str(), idr.n(), idr.avg_ms(),
	        p.avg_ms(), src.errors(), src.concealed());
	summary.push_back(b);
	return quit_poll() == QUIT_NONE;
}

// bgra / yuv: decode, convert, draw, flip; the picture on screen.
bool step_show(Source &src, bool yuv, std::vector<std::string> &summary)
{
	const char *name = yuv ? "yuv" : "bgra";
	ui::show(yuv ? "Software decoder 3/3: colour by the graphics chip"
	             : "Software decoder 2/3: on the screen",
	         std::string(yuv ? "The same video for 10 s; this time the graphics chip converts "
	                           "the colours. Are the colours the same as before, or wrong, "
	                           "striped or garbled?"
	                         : "The 800x480 test video, decoded in software, for 15 s. Does the "
	                           "picture look right (counter in the middle, red band right, blue "
	                           "band at the bottom)? How smooth is it?") +
	             "\n\nFilm the screen. Nothing else to do.",
	         kFooter);
	if (sleep_quit(kLabelMs))
		return false;
	ui::hide();

	log_msg("sw %s: open", name);
	GfOut out;
	if (!out.open()) {
		log_msg("FAIL sw %s: could not open the displayable", name);
		summary.push_back(std::string("sw ") + name + ": FAIL, no displayable");
		return quit_poll() == QUIT_NONE;
	}
	std::vector<u8> buf(800 * 480 * 4);
	gf_format_t fmt = yuv ? GF_FORMAT_PLANAR_YUV_420 : GF_FORMAT_BGRA8888;
	const char *fmt_name = yuv ? "planar I420" : "BGRA8888";
	Series dec, conv, draw, flip, frame;
	swdec_pic pic;
	unsigned t0 = log_ms(), win = t0, limit = yuv ? kYuvMs : kBgraMs;
	usize win_n = 0, shown = 0, failed = 0;
	std::string fail;
	while (quit_poll() == QUIT_NONE && log_ms() - t0 < limit) {
		unsigned f0 = log_us();
		u32 us, cus = 0, dus = 0, fus = 0;
		if (!src.next(&pic, us))
			continue;
		dec.add(us);
		if (pic.crop_w > 800 || pic.crop_h > 480) {
			fail = "picture bigger than 800x480";
			break;
		}
		const u8 *img = buf.data();
		int stride = pic.crop_w * 4;
		unsigned c0 = log_us();
		if (fmt == GF_FORMAT_PLANAR_YUV_420) {
			img = pic.i420;
			stride = pic.width;
		} else if (fmt == GF_FORMAT_PACK_YUV_UYVY) {
			to_uyvy(pic, buf.data());
			stride = pic.crop_w * 2;
		} else {
			swdec_to_bgra(&pic, (u32 *)buf.data(), pic.crop_w);
		}
		cus = log_us() - c0;
		int rc = out.draw(img, fmt, stride, pic.crop_w, pic.crop_h, dus, fus);
		if (shown == 0 && failed == 0)
			log_msg("  first draw: %s, gf_draw_image %d, draw %.1f ms, flip %.1f ms", fmt_name, rc,
			        dus / 1000.0, fus / 1000.0);
		if (rc != 0) {
			failed++;
			if (fmt == GF_FORMAT_PLANAR_YUV_420 && shown == 0) {
				log_msg("  libgf refuses planar I420 (%d): UYVY packed by the CPU instead", rc);
				fmt = GF_FORMAT_PACK_YUV_UYVY;
				fmt_name = "UYVY (planar I420 refused)";
				continue;
			}
			char b[80];
			std::snprintf(b, sizeof(b), "gf_draw_image/flip %d (%s)", rc, fmt_name);
			fail = b;
			break;
		}
		shown++;
		conv.add(cus);
		draw.add(dus);
		flip.add(fus);
		frame.add(log_us() - f0);
		win_n++;
		if (log_ms() - win >= kWindowMs) {
			log_msg("  sw %s t=%u s: %.1f fps, avg ms decode %.1f convert %.1f draw %.1f flip %.1f",
			        name, (log_ms() - t0) / 1000, fps(win_n, log_ms() - win), dec.avg_ms(),
			        conv.avg_ms(), draw.avg_ms(), flip.avg_ms());
			win = log_ms();
			win_n = 0;
		}
	}
	unsigned ms = log_ms() - t0;
	out.close();
	log_msg("RESULT sw_%s format=%s shown=%zu ms=%u fps=%.1f frame_ms=%s decode_ms=%s convert_ms=%s "
	        "draw_ms=%s flip_ms=%s errors=%u concealed=%u%s%s",
	        name, fmt_name, shown, ms, fps(shown, ms), frame.spread().c_str(), dec.spread().c_str(),
	        conv.spread().c_str(), draw.spread().c_str(), flip.spread().c_str(), src.errors(),
	        src.concealed(), fail.empty() ? "" : " FAIL ", fail.c_str());
	char b[220];
	if (fail.empty() || shown > 0)
		std::snprintf(b, sizeof(b), "sw %s (%s): %.1f fps = decode %.1f + convert %.1f + draw %.1f "
		              "+ flip %.1f ms%s",
		              name, fmt_name, fps(shown, ms), dec.avg_ms(), conv.avg_ms(), draw.avg_ms(),
		              flip.avg_ms(), fail.empty() ? "" : ", then FAILED");
	else
		std::snprintf(b, sizeof(b), "sw %s: FAIL %s", name, fail.c_str());
	summary.push_back(b);
	return quit_poll() == QUIT_NONE;
}

} // namespace

bool run(const u8 *data, const std::vector<h264::Range> &frames, std::vector<std::string> &summary)
{
	Source src(data, frames);
	if (frames.empty() || !src.open()) {
		log_msg("FAIL sw: %s", frames.empty() ? "no frames" : "decoder: out of memory");
		summary.push_back("sw: FAIL, no decoder");
		return quit_poll() == QUIT_NONE;
	}
	return step_decode(src, summary) && step_show(src, false, summary) &&
	       step_show(src, true, summary);
}

} // namespace soft

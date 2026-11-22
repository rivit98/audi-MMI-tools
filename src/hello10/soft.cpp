// soft.cpp - hello10's sw tests (soft.h): software H.264 decoding and every
// way of putting the picture on screen, each method a sub-test.
#include "soft.h"

extern "C" {
#include "log.h"
#include "quit.h"
#include "lmgr.h"
#include "lmstate.h"
}

#include "readback.h"
#include "swdec.h"
#include "ui.h"

#define _Static_assert static_assert   // the SDK's gf.h checks its layouts the C way
extern "C" {
#include <gf/gf.h>
#include "gfx.h"
#define GL_GLEXT_PROTOTYPES
#include <GLES/gl.h>
#include <GLES/glext.h>
}
#undef _Static_assert

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <sys/mman.h>
#include <unistd.h>

// libgf's 2D drawing calls; sdk/include/gf/gf.h declares only what the
// other apps use. All are exported by the unit's libgf.so.1 (stubbed by the
// SDK). gf_surface_attach and gf_draw_blit2 as layermanagerV2 calls them
// (P1001 0x08053ed8: surface, dev, w, h, stride, format, palette, pixels, 0;
// blit2: context, source, destination, x1, y1, x2, y2 inclusive, dx, dy).
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
int gf_surface_attach(gf_surface_t *psurface, gf_dev_t gdev, int w, int h, int stride,
                      gf_format_t format, const gf_palette_t *palette, u8 *pixels, unsigned flags);
int gf_draw_blit2(gf_context_t context, gf_surface_t src, gf_surface_t dst, int sx1, int sy1,
                  int sx2, int sy2, int dx, int dy);
}

namespace soft {
namespace {

constexpr int kCid = 40;            // the apps' displayable (screen.c's)
constexpr int kRegisterFlags = 2;   // showScreen's
constexpr int kScreenW = 800, kScreenH = 480;
constexpr int kTexW = 1024, kTexH = 512;
constexpr unsigned kDecodeMs = 15000;
constexpr unsigned kShowMs = 8000;
constexpr unsigned kWindowMs = 5000;
constexpr unsigned kLabelMs = 7000;
constexpr usize kCheckEvery = 25;   // pictures between two read-back checks
constexpr const char *kFooter = "hello10 - software decoder. Pull the SD card to stop.";

enum class Method { Decode, Cpu, Image, Image565, Yuv, Blit, Gl, Gl565 };

struct MethodInfo {
	const char *name;
	Method method;
	const char *title;
	const char *text;
	int tol;   // read-back tolerance (colour difference still "right")
};

const MethodInfo kMethods[] = {
	{"dec", Method::Decode, "decoding only",
	 "The MMI decodes the 800x480 test video in software for 15 s, as fast as it can. "
	 "Nothing moves on the screen. Nothing to do.",
	 0},
	{"cpu", Method::Cpu, "drawn by the processor",
	 "The video, the processor writing every pixel into the screen itself (slow, a few "
	 "pictures a second). Does the picture show?",
	 2},
	{"image", Method::Image, "copied by the graphics chip (draw image)",
	 "The video, the graphics chip copying each picture to the screen. Does the picture "
	 "show? How smooth is it?",
	 2},
	{"image565", Method::Image565, "16-bit colour, copied by the graphics chip",
	 "The same with fewer colours. Does the picture show, colours right?", 12},
	{"yuv", Method::Yuv, "colour by the graphics chip",
	 "The graphics chip converts the colours too. Are the colours right, or wrong, striped "
	 "or garbled?",
	 24},
	{"blit", Method::Blit, "copied by the graphics chip (blit)",
	 "The video copied to the screen another way. Does the picture show? How smooth?", 2},
	{"gl", Method::Gl, "OpenGL texture",
	 "The video drawn through OpenGL. Does the picture show? How smooth is it?", 2},
	{"gl565", Method::Gl565, "OpenGL texture, 16-bit colour",
	 "The same with fewer colours. Does the picture show, colours right?", 12},
};

const MethodInfo *find_method(const std::string &name)
{
	for (const MethodInfo &m : kMethods)
		if (name == m.name)
			return &m;
	return nullptr;
}

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

bool write_file(const std::string &path, const char *data, usize n)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = std::fwrite(data, 1, n, f) == n;
	return std::fclose(f) == 0 && ok;
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

// One picture's way to the screen, timed in parts (microseconds).
struct Times {
	u32 convert = 0;   // colour conversion / packing by the CPU
	u32 draw = 0;      // into the surface: CPU writes, 2D engine, GL upload + quad
	u32 flip = 0;      // updateVfb + getVfb, or the GL swap
	u32 check = 0;     // read-back (left out of the frame time)
};

// A way of putting pictures on screen.
class Display {
public:
	virtual ~Display() {}
	virtual bool open() = 0;
	// Puts p on screen. check: read sample pixels back into r first.
	// Returns 0, or the failing call's code (the sub-test stops).
	virtual int show(const swdec_pic &p, bool check, readback::Result &r, Times &t) = 0;
	// The format in use, for the log ("BGRA8888", "UYVY (I420 refused)").
	virtual std::string format() const = 0;
	// After the timed part: state dump (and d-shot) into dir.
	virtual void diagnose(const std::string &name, const std::string &dir) = 0;
	virtual void close() = 0;
};

// The layer manager's state: saved as lm_<name>.txt, the lines that matter logged.
void dump_lm_state(const std::string &name, const std::string &dir)
{
	static char buf[16384];
	usize len = 0;
	int rc = lm_read_state(buf, sizeof(buf), &len, nullptr);
	if (rc != 0 && rc != EOVERFLOW) {
		log_msg("  lm state: %s", std::strerror(rc));
		return;
	}
	std::string path = dir + "/lm_" + name + ".txt";
	bool saved = write_file(path, buf, len);
	lm_state_displayable d[48];
	int n = std::min(lm_state_displayables(buf, d, 48), 48);
	std::string mine = "cid 40 not in the dump";
	for (int i = 0; i < n; i++)
		if (d[i].cid == kCid) {
			char b[120];
			std::snprintf(b, sizeof(b), "cid 40 front sid %d back sid %d visible on gf index %d",
			              d[i].front_sid, d[i].back_sid, d[i].visible_gf_index);
			mine = b;
		}
	log_msg("  lm state -> %s%s: layer 0 shows cid %d; %s", path.c_str(),
	        saved ? "" : " (not saved)", lm_state_layer_cid(buf, 0), mine.c_str());
}

// cid 40 through the layer manager, drawn by the CPU or the 2D engine. The
// VFB is double-buffered (its sid changes on every updateVfb), so up to two
// surfaces stay attached, each with its own context.
class LmDisplay : public Display {
public:
	explicit LmDisplay(Method m) : m_(m) {}
	~LmDisplay() override { close(); }

	bool open() override
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
		buf_.assign((usize)kScreenW * kScreenH * 4, 0);
		return rc == 0;
	}

	int show(const swdec_pic &p, bool check, readback::Result &r, Times &t) override
	{
		Slot *s = slot_for(sid_);
		if (s == nullptr)
			return -1;
		if (p.crop_w > s->si.w || p.crop_h > s->si.h)
			return -2;
		int rc = 0;
		unsigned t0 = log_us();
		switch (m_) {
		case Method::Cpu:
			swdec_to_bgra(&p, (u32 *)s->si.vaddr, (int)(s->si.stride / 4));
			t.draw = log_us() - t0;
			break;
		case Method::Image:
			swdec_to_bgra(&p, (u32 *)buf_.data(), p.crop_w);
			t.convert = log_us() - t0;
			rc = image(s, buf_.data(), GF_FORMAT_BGRA8888, p.crop_w * 4, p, t);
			break;
		case Method::Image565:
			swdec_to_rgb565(&p, (u16 *)buf_.data(), p.crop_w);
			t.convert = log_us() - t0;
			rc = image(s, buf_.data(), GF_FORMAT_PACK_RGB565, p.crop_w * 2, p, t);
			break;
		case Method::Yuv:
			if (!uyvy_) {
				// planar: Y with the luma stride, U and V after it (coded size)
				rc = image(s, p.i420, GF_FORMAT_PLANAR_YUV_420, p.width, p, t);
				if (rc == GF_ERR_OK || drawn_ > 0)
					break;
				log_msg("  libgf refuses planar I420 (%d): UYVY packed by the CPU instead", rc);
				uyvy_ = true;
				t0 = log_us();
			}
			to_uyvy(p, buf_.data());
			t.convert = log_us() - t0;
			rc = image(s, buf_.data(), GF_FORMAT_PACK_YUV_UYVY, p.crop_w * 2, p, t);
			break;
		case Method::Blit:
			rc = blit(s, p, t);
			break;
		default:
			return -1;
		}
		if (rc != GF_ERR_OK)
			return rc;
		if (check && s->si.vaddr != nullptr) {
			unsigned c0 = log_us();
			const u8 *base = s->si.vaddr;
			usize stride = s->si.stride;
			readback::check(p, tol_, [&](int x, int y) {
				return *(const volatile u32 *)(base + (usize)y * stride + (usize)x * 4);
			}, r);
			t.check = log_us() - c0;
		}
		unsigned f0 = log_us();
		lm_vfb v;
		int lrc = lm_update_vfb(kCid);
		if (lrc == 0)
			lrc = lm_get_vfb(kCid, &v);
		t.flip = log_us() - f0;
		if (lrc != 0) {
			log_msg("  updateVfb/getVfb: %s", std::strerror(lrc));
			return lrc;
		}
		sid_ = v.sid;
		drawn_++;
		return 0;
	}

	std::string format() const override
	{
		switch (m_) {
		case Method::Cpu: return "BGRA8888 written by the CPU";
		case Method::Image: return "BGRA8888";
		case Method::Image565: return "PACK_RGB565";
		case Method::Yuv: return uyvy_ ? "UYVY (planar I420 refused)" : "planar I420";
		case Method::Blit: return "BGRA8888 RAM surface";
		default: return "?";
		}
	}

	void set_tolerance(int tol) { tol_ = tol; }

	void diagnose(const std::string &name, const std::string &dir) override
	{
		dump_lm_state(name, dir);
		// what the layer manager has for cid 40 (~3 s): our picture, the old
		// text screen or noise
		std::string path = dir + "/dshot_" + name + ".png";
		unsigned t0 = log_ms();
		int rc = lm_text_command(("d-shot 40 " + path).c_str());
		log_msg("  d-shot 40 -> %s: rc=%d, %u ms", path.c_str(), rc, log_ms() - t0);
	}

	void close() override
	{
		if (src_ != nullptr)
			gf_surface_free(src_);
		src_ = nullptr;
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
		std::free(ram_);
		ram_ = nullptr;
	}

private:
	struct Slot {
		unsigned sid = 0;
		gf_surface_t surface = nullptr;
		gf_surface_info_t si;
		gf_context_t ctx = nullptr;
		unsigned used = 0;
	};

	int image(Slot *s, const u8 *img, gf_format_t fmt, int stride, const swdec_pic &p, Times &t)
	{
		unsigned t0 = log_us();
		int rc = gf_draw_begin(s->ctx);
		if (rc == GF_ERR_OK) {
			rc = gf_draw_image(s->ctx, img, fmt, stride, nullptr, 0, 0, p.crop_w, p.crop_h, 0);
			int fin = gf_draw_finish(s->ctx);
			gf_draw_end(s->ctx);
			if (rc == GF_ERR_OK)
				rc = fin;
		}
		t.draw = log_us() - t0;
		if (drawn_ == 0)
			log_msg("  first gf_draw_image(%d): %d, %.1f ms", (int)fmt, rc, t.draw / 1000.0);
		return rc;
	}

	// The RAM picture as a gf surface, made on the first frame (the size
	// comes with the picture) and reused.
	int blit(Slot *s, const swdec_pic &p, Times &t)
	{
		usize bytes = (usize)p.crop_w * p.crop_h * 4;
		if (src_ == nullptr) {
			if (posix_memalign((void **)&ram_, 32, bytes) != 0) {
				ram_ = nullptr;
				return GF_ERR_MEM;
			}
			std::memset(ram_, 0, bytes);
			int rc = gf_surface_attach(&src_, dev_, p.crop_w, p.crop_h, p.crop_w * 4,
			                           GF_FORMAT_BGRA8888, nullptr, ram_, 0);
			gf_surface_info_t si;
			std::memset(&si, 0, sizeof(si));
			if (rc == GF_ERR_OK)
				gf_surface_get_info(src_, &si);
			off64_t off = 0;
			usize contig = 0;
			int ofd = -1;
			int mrc = posix_mem_offset64(ram_, bytes, &off, &contig, &ofd);
			log_msg("  gf_surface_attach(RAM %dx%d BGRA): %d sid %u stride %u vaddr %p paddr "
			        "0x%llx; posix_mem_offset: rc=%d 0x%llx contiguous %zu of %zu",
			        p.crop_w, p.crop_h, rc, si.sid, si.stride, (void *)si.vaddr,
			        (unsigned long long)si.paddr, mrc, (unsigned long long)off, contig, bytes);
			if (rc != GF_ERR_OK) {
				src_ = nullptr;
				return rc;
			}
			src_w_ = p.crop_w;
			src_h_ = p.crop_h;
		}
		if (p.crop_w != src_w_ || p.crop_h != src_h_)
			return -3;
		unsigned t0 = log_us();
		swdec_to_bgra(&p, (u32 *)ram_, p.crop_w);
		t.convert = log_us() - t0;
		t0 = log_us();
		// the 2D engine may read RAM past the CPU's cache: write it back
		int mrc = msync(ram_, bytes, MS_SYNC | MS_CACHE_ONLY);
		int rc = gf_draw_begin(s->ctx);
		if (rc == GF_ERR_OK) {
			rc = gf_draw_blit2(s->ctx, src_, s->surface, 0, 0, p.crop_w - 1, p.crop_h - 1, 0, 0);
			int fin = gf_draw_finish(s->ctx);
			gf_draw_end(s->ctx);
			if (rc == GF_ERR_OK)
				rc = fin;
		}
		t.draw = log_us() - t0;
		if (drawn_ == 0)
			log_msg("  first gf_draw_blit2: %d, %.1f ms (msync %d)", rc, t.draw / 1000.0, mrc);
		return rc;
	}

	// I420 (coded size) -> UYVY bytes, the visible part.
	static void to_uyvy(const swdec_pic &p, u8 *out)
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

	// The attached VFB surface for sid (+ its context unless the CPU draws).
	Slot *slot_for(unsigned sid)
	{
		for (Slot &s : slots_)
			if (s.surface != nullptr && s.sid == sid) {
				s.used = ++tick_;
				return &s;
			}
		Slot &s = slots_[0].used <= slots_[1].used ? slots_[0] : slots_[1];
		if (s.ctx != nullptr)
			gf_context_free(s.ctx);
		if (s.surface != nullptr)
			gf_surface_free(s.surface);
		s = Slot();
		std::memset(&s.si, 0, sizeof(s.si));
		int rc = gf_surface_attach_by_sid(&s.surface, dev_, sid);
		if (rc == GF_ERR_OK)
			gf_surface_get_info(s.surface, &s.si);
		bool cpu_ok = s.si.vaddr != nullptr && s.si.stride >= (unsigned)s.si.w * 4;
		int crc = 0, src = 0;
		if (rc == GF_ERR_OK && m_ != Method::Cpu) {
			crc = gf_context_create(&s.ctx);
			src = crc == GF_ERR_OK ? gf_context_set_surface(s.ctx, s.surface) : -1;
		}
		log_msg("  sid %u: attach %d (%dx%d format %d stride %u vaddr %p paddr 0x%llx), "
		        "context %d, set_surface %d",
		        sid, rc, s.si.w, s.si.h, (int)s.si.format, s.si.stride, (void *)s.si.vaddr,
		        (unsigned long long)s.si.paddr, crc, src);
		if (!cpu_ok) {
			log_msg("  sid %u: no 32-bit CPU mapping (no read-back%s)", sid,
			        m_ == Method::Cpu ? ", no CPU drawing" : "");
			s.si.vaddr = nullptr;
		}
		if (rc != GF_ERR_OK || crc != GF_ERR_OK || src != GF_ERR_OK ||
		    (!cpu_ok && m_ == Method::Cpu)) {
			if (s.ctx != nullptr)
				gf_context_free(s.ctx);
			if (s.surface != nullptr)
				gf_surface_free(s.surface);
			s = Slot();
			return nullptr;
		}
		s.sid = sid;
		s.used = ++tick_;
		return &s;
	}

	Method m_;
	int tol_ = 2;
	gf_dev_t dev_ = nullptr;
	gf_display_t display_ = nullptr;
	bool lm_open_ = false, registered_ = false, uyvy_ = false;
	unsigned sid_ = 0, tick_ = 0;
	usize drawn_ = 0;
	Slot slots_[2];
	std::vector<u8> buf_;
	// blit: the RAM picture and its gf surface
	u8 *ram_ = nullptr;
	gf_surface_t src_ = nullptr;
	int src_w_ = 0, src_h_ = 0;
};

// GL (common/gfx.c): a texture the size of the screen rounded up to powers
// of two, one upload in flight (glFinish after each: queued uploads froze the
// unit, docs/gpu-results.md 6), a textured quad, swap.
class GlDisplay : public Display {
public:
	explicit GlDisplay(bool rgb565) : rgb565_(rgb565) {}
	~GlDisplay() override { close(); }

	bool open() override
	{
		std::memset(&g_, 0, sizeof(g_));
		started_ = true;
		if (gfx_init(&g_, kCid) != 0) {
			log_msg("  gfx_init failed (the lines above)");
			return false;
		}
		glViewport(0, 0, kScreenW, kScreenH);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrthof(0.0f, (GLfloat)kScreenW, 0.0f, (GLfloat)kScreenH, -1.0f, 1.0f);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_LIGHTING);
		glEnable(GL_TEXTURE_2D);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glClearColor(0, 0, 0, 1);

		glGenTextures(1, &tex_);
		glBindTexture(GL_TEXTURE_2D, tex_);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		std::vector<u8> zero((usize)kTexW * kTexH * 4, 0);
		log_msg("  glTexImage2D %dx%d %s", kTexW, kTexH, rgb565_ ? "RGB565" : "RGBA");
		if (rgb565_)
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kTexW, kTexH, 0, GL_RGB,
			             GL_UNSIGNED_SHORT_5_6_5, zero.data());
		else
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTexW, kTexH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			             zero.data());
		glFinish();
		GLenum err = glGetError();
		log_msg("  glTexImage2D: glError 0x%04x", err);
		buf_.assign((usize)kScreenW * kScreenH * 4, 0);
		return err == GL_NO_ERROR;
	}

	int show(const swdec_pic &p, bool check, readback::Result &r, Times &t) override
	{
		if (p.crop_w > kScreenW || p.crop_h > kScreenH)
			return -2;
		unsigned t0 = log_us();
		if (rgb565_)
			swdec_to_rgb565(&p, (u16 *)buf_.data(), p.crop_w);
		else
			swdec_to_rgba(&p, (u32 *)buf_.data(), p.crop_w);
		t.convert = log_us() - t0;

		t0 = log_us();
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p.crop_w, p.crop_h, rgb565_ ? GL_RGB : GL_RGBA,
		                rgb565_ ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, buf_.data());
		glFinish();   // one upload in flight, always
		// the picture's top-left at the screen's top-left (GL's y goes up)
		const GLfloat x1 = (GLfloat)p.crop_w, y0 = (GLfloat)(kScreenH - p.crop_h),
		              y1 = (GLfloat)kScreenH;
		const GLfloat v[8] = {0, y0, x1, y0, 0, y1, x1, y1};
		const GLfloat s = (GLfloat)p.crop_w / kTexW, tt = (GLfloat)p.crop_h / kTexH;
		const GLfloat tc[8] = {0, tt, s, tt, 0, 0, s, 0};   // texture row 0 = top line
		glClear(GL_COLOR_BUFFER_BIT);
		glVertexPointer(2, GL_FLOAT, 0, v);
		glTexCoordPointer(2, GL_FLOAT, 0, tc);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		t.draw = log_us() - t0;
		if (frames_ == 0) {
			GLenum err = glGetError();
			log_msg("  first upload + quad: glError 0x%04x, %.1f ms", err, t.draw / 1000.0);
			if (err != GL_NO_ERROR)
				return (int)err;
		}

		if (check) {
			unsigned c0 = log_us();
			readback::check(p, tol_, [&](int x, int y) {
				u8 px[4] = {0, 0, 0, 0};
				glReadPixels(x, kScreenH - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
				return (u32)px[0] << 16 | (u32)px[1] << 8 | px[2];
			}, r);
			t.check = log_us() - c0;
		}
		t0 = log_us();
		int rc = gfx_swap(&g_);
		t.flip = log_us() - t0;
		frames_++;
		return rc;
	}

	std::string format() const override
	{
		return rgb565_ ? "GL RGB565 texture" : "GL RGBA texture";
	}

	void set_tolerance(int tol) { tol_ = tol; }

	void diagnose(const std::string &name, const std::string &dir) override
	{
		// a d-shot of a GL displayable shows noise, never GL's picture
		// (docs/gpu-results.md 2): the state dump only
		dump_lm_state(name, dir);
	}

	void close() override
	{
		if (!started_)
			return;
		if (tex_ != 0)
			glDeleteTextures(1, &tex_);
		tex_ = 0;
		gfx_shutdown(&g_);
		started_ = false;
	}

private:
	bool rgb565_;
	int tol_ = 2;
	bool started_ = false;
	gfx g_;
	GLuint tex_ = 0;
	usize frames_ = 0;
	std::vector<u8> buf_;
};

// dec: as fast as it goes, no picture.
bool step_decode(Source &src, std::vector<std::string> &summary)
{
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
			log_msg("  sw dec t=%u s: %.1f fps", (log_ms() - t0) / 1000,
			        fps(win_n, log_ms() - win));
			win = log_ms();
			win_n = 0;
		}
	}
	unsigned ms = log_ms() - t0;
	char b[200];
	std::snprintf(b, sizeof(b), "sw dec: %.1f fps, %zu frames, ms %s (IDR %.1f, P %.1f)",
	              fps(all.n(), ms), all.n(), all.spread().c_str(), idr.avg_ms(), p.avg_ms());
	log_msg("RESULT sw_dec frames=%zu ms=%u fps=%.1f decode_ms(min/med/p90/max)=%s "
	        "idr=%zu idr_avg=%.1f p_avg=%.1f errors=%u concealed=%u",
	        all.n(), ms, fps(all.n(), ms), all.spread().c_str(), idr.n(), idr.avg_ms(),
	        p.avg_ms(), src.errors(), src.concealed());
	summary.push_back(b);
	return quit_poll() == QUIT_NONE;
}

std::unique_ptr<Display> make_display(const MethodInfo &m)
{
	switch (m.method) {
	case Method::Gl:
	case Method::Gl565: {
		auto d = std::make_unique<GlDisplay>(m.method == Method::Gl565);
		d->set_tolerance(m.tol);
		return d;
	}
	default: {
		auto d = std::make_unique<LmDisplay>(m.method);
		d->set_tolerance(m.tol);
		return d;
	}
	}
}

// One screen method: decode, convert, draw, flip for kShowMs; the picture
// read back every kCheckEvery pictures.
bool step_show(Source &src, const MethodInfo &m, const std::string &out_dir,
               std::vector<std::string> &summary)
{
	std::string name = std::string("sw ") + m.name;
	std::unique_ptr<Display> out = make_display(m);
	log_msg("%s: open", name.c_str());
	if (!out->open()) {
		out->close();
		log_msg("RESULT sw_%s FAIL could not open the display path", m.name);
		summary.push_back(name + ": FAIL, could not open");
		return quit_poll() == QUIT_NONE;
	}
	Series dec, conv, draw, flip, frame;
	readback::Result rb;
	swdec_pic pic;
	unsigned t0 = log_ms(), win = t0;
	usize win_n = 0, shown = 0;
	std::string fail;
	while (quit_poll() == QUIT_NONE && log_ms() - t0 < kShowMs) {
		unsigned f0 = log_us();
		u32 us;
		if (!src.next(&pic, us))
			continue;
		dec.add(us);
		Times t;
		bool check = shown % kCheckEvery == 0;
		int checks = rb.checks;
		int rc = out->show(pic, check, rb, t);
		if (check && rb.checks > checks && shown == 0)
			log_msg("  first read-back: %s", readback::str(rb).c_str());
		if (rc != 0) {
			char b[80];
			std::snprintf(b, sizeof(b), "draw/flip failed: %d (%s)", rc, out->format().c_str());
			fail = b;
			break;
		}
		shown++;
		conv.add(t.convert);
		draw.add(t.draw);
		flip.add(t.flip);
		frame.add(log_us() - f0 - t.check);
		win_n++;
		if (log_ms() - win >= kWindowMs) {
			log_msg("  %s t=%u s: %.1f fps, avg ms decode %.1f convert %.1f draw %.1f flip %.1f",
			        name.c_str(), (log_ms() - t0) / 1000, fps(win_n, log_ms() - win),
			        dec.avg_ms(), conv.avg_ms(), draw.avg_ms(), flip.avg_ms());
			win = log_ms();
			win_n = 0;
		}
	}
	unsigned ms = log_ms() - t0;
	// fps without the read-back time (checks stall the loop a little)
	f64 loop_ms = frame.avg_ms() * (f64)frame.n();
	f64 rate = loop_ms > 0 ? (f64)frame.n() * 1000.0 / loop_ms : 0;
	if (shown > 0)
		out->diagnose(m.name, out_dir);
	out->close();
	log_msg("RESULT sw_%s format=\"%s\" shown=%zu ms=%u fps=%.1f frame_ms=%s decode_ms=%s "
	        "convert_ms=%s draw_ms=%s flip_ms=%s readback=%s %s errors=%u concealed=%u%s%s",
	        m.name, out->format().c_str(), shown, ms, rate, frame.spread().c_str(),
	        dec.spread().c_str(), conv.spread().c_str(), draw.spread().c_str(),
	        flip.spread().c_str(), readback::verdict(rb), readback::str(rb).c_str(),
	        src.errors(), src.concealed(), fail.empty() ? "" : " FAIL ", fail.c_str());
	char b[240];
	if (shown > 0)
		std::snprintf(b, sizeof(b), "%s (%s): %.1f fps = dec %.1f + conv %.1f + draw %.1f + flip "
		              "%.1f ms, readback %s%s",
		              name.c_str(), out->format().c_str(), rate, dec.avg_ms(), conv.avg_ms(),
		              draw.avg_ms(), flip.avg_ms(), readback::verdict(rb),
		              fail.empty() ? "" : ", then FAILED");
	else
		std::snprintf(b, sizeof(b), "%s: FAIL %s", name.c_str(), fail.c_str());
	summary.push_back(b);
	return quit_poll() == QUIT_NONE;
}

std::vector<std::string> split(const std::string &list)
{
	std::vector<std::string> out;
	usize i = 0;
	while (i <= list.size()) {
		usize j = list.find(',', i);
		if (j == std::string::npos)
			j = list.size();
		if (j > i)
			out.push_back(list.substr(i, j - i));
		i = j + 1;
	}
	return out;
}

} // namespace

const std::vector<std::string> &all_methods()
{
	static const std::vector<std::string> names = [] {
		std::vector<std::string> v;
		for (const MethodInfo &m : kMethods)
			v.push_back(m.name);
		return v;
	}();
	return names;
}

bool run(const u8 *data, const std::vector<h264::Range> &frames, const std::string &methods,
         const std::string &out_dir, std::vector<std::string> &summary)
{
	std::vector<const MethodInfo *> run;
	for (const std::string &n : split(methods)) {
		const MethodInfo *m = find_method(n);
		if (m == nullptr)
			log_msg("sw: no method \"%s\", skipped", n.c_str());
		else
			run.push_back(m);
	}
	Source src(data, frames);
	if (frames.empty() || !src.open()) {
		log_msg("FAIL sw: %s", frames.empty() ? "no frames" : "decoder: out of memory");
		summary.push_back("sw: FAIL, no decoder");
		return quit_poll() == QUIT_NONE;
	}
	int n = 0;
	for (const MethodInfo *m : run) {
		char title[120];
		std::snprintf(title, sizeof(title), "Software decoder %d/%zu: %s", ++n, run.size(),
		              m->title);
		log_msg("== sw %s", m->name);
		ui::show(title,
		         std::string(m->text) +
		             (m->method == Method::Decode ? "" : "\n\nFilm the screen. Nothing else to do."),
		         kFooter);
		if (m->method == Method::Decode) {
			if (!step_decode(src, summary))
				return false;
			continue;
		}
		if (sleep_quit(kLabelMs))
			return false;
		ui::hide();
		if (!step_show(src, *m, out_dir, summary))
			return false;
	}
	ui::hide();
	return quit_poll() == QUIT_NONE;
}

} // namespace soft

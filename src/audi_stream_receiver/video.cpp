// video.cpp - picture hand-over, decode and acks (see video.h).
#include "video.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
#include "imgdec.h"
#include "log.h"
}

#include "aoa.h"
#include "hwvideo.h"
#include "jpeg_pacer.h"
#include "mono_cond.h"
#include "perf.h"

using proto::PicStatus;

namespace video {

namespace {

constexpr unsigned kFpsWindowMs = 10000;
constexpr unsigned kLogFirst = 3;   // pictures logged one by one; then perf.cpp's summaries
constexpr unsigned kShownRing = 64;

int land_w = 800, land_h = 480, port_w = 400, port_h = 480;

// --- shared with the usbd thread (mu) -----------------------------------
std::mutex  mu;
MonoCond    work;                 // a picture or a stop is waiting
Picture     waiting;              // data == nullptr: none
bool        stop_pending;
unsigned    dropped;              // replaced while waiting
std::string stop_why;

// --- main loop only -----------------------------------------------------
struct Free {
	void operator()(uint32_t *p) const { std::free(p); }
};
View      cur;
std::unique_ptr<uint32_t, Free> cur_px;   // cur.frame.px (imgdec's malloc)
bool      have_view;
bool      unacked;                // cur.id decoded, not on screen yet
bool      held;                   // cur.id on screen, its ack held back by the cap
unsigned  held_since_us;          // log_us() when it was shown
JpegPacer pacer;                  // the cap: kMaxJpegFps, ~50 % duty
unsigned  shown_at[kShownRing];   // log_ms() of recent pictures, a ring
unsigned  shown_n;
perf::Pic rec;                    // cur's timings so far

// A picture that never reached the screen.
void perf_lost(const Picture &pic, PicStatus status)
{
	perf::Pic r;

	r.t_ms = log_ms();
	r.id = pic.id;
	r.bytes = pic.size;
	r.w = pic.w;
	r.h = pic.h;
	r.phone_ms = pic.phone_ms;
	r.status = status;
	r.rx_us = pic.rx_done_us - pic.rx_start_us;
	perf::add(r);
}

// The one on screen never got there.
void drop_unacked()
{
	if (!unacked)
		return;
	aoa::ack(PicStatus::Dropped);
	rec.status = PicStatus::Dropped;
	rec.t_ms = log_ms();
	perf::add(rec);
	unacked = false;
}

// The ack of the picture on screen goes out: the phone may send the next.
void release_held()
{
	if (!held)
		return;
	held = false;
	aoa::ack(PicStatus::Shown);
	rec.hold_us = log_us() - held_since_us;
	perf::add(rec);
}

unsigned fps_x10(unsigned now)
{
	unsigned n = 0, count = shown_n < kShownRing ? shown_n : kShownRing;

	for (unsigned i = 0; i < count; i++)
		if (now - shown_at[i] < kFpsWindowMs)
			n++;
	return n * 10000 / kFpsWindowMs;
}

} // namespace

void init(int screen_w, int screen_h)
{
	land_w = screen_w;
	land_h = screen_h;                   // the status bar goes over it
	port_w = screen_w / 2;
	port_h = screen_h;
	log_msg("video: boxes landscape %dx%d, portrait %dx%d, max %u bytes",
	        land_w, land_h, port_w, port_h, (unsigned)kMaxBytes);
}

std::array<uint8_t, proto::kVideoCapsLen> caps()
{
	std::array<uint8_t, proto::kVideoCapsLen> a;
	uint8_t *out = a.data();

	proto::put_u16(out, (unsigned)land_w);
	proto::put_u16(out + 2, (unsigned)land_h);
	proto::put_u16(out + 4, (unsigned)port_w);
	proto::put_u16(out + 6, (unsigned)port_h);
	proto::put_u32(out + 8, kMaxBytes);
	out[12] = 1u << (proto::kFmtJpeg - 1) | 1u << (proto::kFmtH264 - 1);
	proto::put_u16(out + 13, hwvideo::kMaxW);
	proto::put_u16(out + 15, hwvideo::kMaxH);
	out[17] = hwvideo::kFps;
	proto::put_u32(out + 18, hwvideo::kMaxFrame);
	return a;
}

void submit(Picture pic)
{
	Picture old;
	{
		std::lock_guard<std::mutex> lock(mu);
		old = std::move(waiting);
		waiting = std::move(pic);
		stop_pending = false;            // a picture after a stop: streaming again
		if (old.data)
			dropped++;
	}
	work.notify();
	if (old.data) {
		aoa::ack(PicStatus::Dropped);
		perf_lost(old, PicStatus::Dropped);
	}
}

void wait(unsigned ms)
{
	// a held ack and a picture held back by the cap are due at the
	// pacer's time: wake for it, not on the picture's arrival
	unsigned until = pacer.wait_ms(log_ms());
	if ((held || until > 0) && until < ms)
		ms = until;
	std::unique_lock<std::mutex> lock(mu);
	work.wait_ms(lock, ms, [] {
		return stop_pending || (waiting.data != nullptr && pacer.ready(log_ms()));
	});
}

void stop(const char *why)
{
	Picture old;
	{
		std::lock_guard<std::mutex> lock(mu);
		old = std::move(waiting);
		waiting = Picture();
		stop_pending = true;
		stop_why = why;
	}
	work.notify();
}

bool poll()
{
	Picture pic;
	std::string why;
	bool stopped;
	unsigned now = log_ms();
	bool ready = pacer.ready(now);

	if (ready)
		release_held();
	{
		std::lock_guard<std::mutex> lock(mu);
		stopped = stop_pending;
		stop_pending = false;
		why = stop_why;
		// the cap: a picture waits (undecoded) until the pacer says so;
		// a newer one replaces it meanwhile and it is acked as dropped
		if (ready || stopped) {
			pic = std::move(waiting);
			waiting = Picture();
		}
		cur.dropped = dropped;
		if (stopped)
			dropped = 0;
	}

	if (stopped) {
		bool had = have_view;

		if (have_view)
			log_msg("video: stopped (%s) after %u shown, %u bad, %u dropped",
			        why.c_str(), cur.shown, cur.bad, cur.dropped);
		release_held();
		drop_unacked();
		pacer.reset();
		cur_px.reset();
		have_view = false;
		cur = View();
		shown_n = 0;
		perf::flush();
		if (!pic.data)
			return had;
	}
	if (!pic.data)
		return false;

	if (!have_view && !pacer.active())
		log_msg("video: JPEG fallback, cap %u fps, duty <= 50%%", JpegPacer::kMaxJpegFps);
	// held back by the cap: from its arrival to the pacer's time
	unsigned rx_age_ms = (log_us() - pic.rx_done_us) / 1000, cap_us = 0;
	if (pacer.active()) {
		unsigned rx_done_ms = now - rx_age_ms;
		if ((int)(pacer.next_ms() - rx_done_ms) > 0)
			cap_us = std::min(pacer.next_ms() - rx_done_ms, rx_age_ms) * 1000;
	}
	pacer.taken(now);

	bool portrait = pic.w < pic.h;
	int max_w = portrait ? port_w : land_w, max_h = portrait ? port_h : land_h;
	int w, h;
	uint32_t *px;
	unsigned t0 = log_us();
	if (imgdec_argb(pic.data.get(), pic.size, max_w, max_h, &px, &w, &h) != 0) {
		imgdec_info(pic.data.get(), pic.size, &w, &h);
		log_msg("video: picture %u (%s, %u bytes, %ux%u) not shown: %s", pic.id,
		        proto::fmt_name(pic.format), (unsigned)pic.size, pic.w, pic.h,
		        w > max_w || h > max_h ? "larger than its box" : "not decodable");
		cur.bad++;
		aoa::ack(PicStatus::Bad);
		perf_lost(pic, PicStatus::Bad);
		pacer.done(log_ms(), (log_us() - t0) / 1000);
		return false;
	}
	unsigned us = log_us() - t0;

	drop_unacked();                      // never presented (headless)
	rec = perf::Pic();
	rec.id = pic.id;
	rec.bytes = pic.size;
	rec.w = (unsigned)w;
	rec.h = (unsigned)h;
	rec.phone_ms = pic.phone_ms;
	rec.rx_us = pic.rx_done_us - pic.rx_start_us;
	rec.wait_us = t0 - pic.rx_done_us;
	rec.decode_us = us;
	rec.cap_us = cap_us;
	rec.latency_us = pic.rx_start_us;    // start; the span when shown
	cur_px.reset(px);
	cur.frame.px = px;
	cur.frame.w = w;
	cur.frame.h = h;
	cur.frame.stride = w;
	cur.portrait = w < h;
	cur.id = pic.id;
	cur.bytes = pic.size;
	cur.format = pic.format;
	cur.decode_ms = (us + 500) / 1000;
	have_view = true;
	unacked = true;
	if ((unsigned)w != pic.w || (unsigned)h != pic.h)
		log_msg("video: picture %u announced %ux%u, is %dx%d", pic.id, pic.w, pic.h, w, h);
	return true;
}

void presented(const Present *p)
{
	unsigned now = log_ms();

	if (!unacked)
		return;
	unacked = false;
	unsigned busy_us = rec.decode_us;
	if (p != nullptr)
		busy_us += p->render_us + p->copy_us + p->flip_us;
	pacer.done(now, busy_us / 1000);
	if (p != nullptr) {
		rec.render_us = p->render_us;
		rec.copy_us = p->copy_us;
		rec.flip_us = p->flip_us;
		rec.vfb_bytes = p->vfb_bytes;
		rec.vfb_rows = p->vfb_rows;
		rec.vfb_full = p->vfb_full;
		cur.present_ms = (p->render_us + p->copy_us + p->flip_us + 500) / 1000;
	} else {
		cur.present_ms = 0;
	}
	cur.shown++;
	shown_at[shown_n++ % kShownRing] = now;
	cur.fps_x10 = fps_x10(now);
	rec.t_ms = now;
	rec.status = PicStatus::Shown;
	rec.latency_us = log_us() - rec.latency_us;
	// the ack (and the record) when the cap lets the next one come
	held = true;
	held_since_us = log_us();
	if (pacer.ready(log_ms()))
		release_held();
	if (cur.shown <= kLogFirst)
		log_msg("video: #%u id %u %dx%d %s, %s %u bytes (phone %u ms), decode %u ms, "
		        "present %u ms (render %u, copy %u, flip %u), vfb %u KB", cur.shown,
		        cur.id, cur.frame.w, cur.frame.h,
		        cur.portrait ? "portrait" : "landscape", proto::fmt_name(cur.format),
		        cur.bytes, rec.phone_ms, cur.decode_ms, cur.present_ms, rec.render_us / 1000,
		        rec.copy_us / 1000, rec.flip_us / 1000, rec.vfb_bytes / 1024);
}

const View *view()
{
	return have_view ? &cur : nullptr;
}

} // namespace video

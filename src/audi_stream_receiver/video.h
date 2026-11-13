// video.h - the phone's pictures, between the USB thread and the screen.
//
//   usbd thread    video::submit() a complete picture (vasm.h), video::stop()
//   main loop      video::poll() decodes the newest waiting picture,
//                  ui.cpp draws video::view(), video::presented() acks it
//
// Only the newest picture waits: one that arrives before the previous is
// decoded replaces it (that one is acked as dropped). The phone keeps two
// pictures in flight (one on its way to the screen, one waiting), so the
// main loop never waits for the phone and normally nothing is dropped.
//
// The cap (jpeg_pacer.h): at most 5 pictures a second, and after each as
// much rest as it took the main thread. The shown picture's ack is held
// until the next may be taken, so the phone (two in flight at most) waits
// rather than sending pictures that would be dropped; one that comes anyway
// waits undecoded. wait() wakes at the pacer's time.
//
// Portrait vs landscape is the picture's own shape (w < h = portrait). The
// MMI tells the phone which box each fits in (VIDEO_CAPS); ui.cpp lays the
// screen out to match: landscape on the whole screen (a slim status bar
// over it only on the phone's VIDEO_DETAILS), portrait on the left with
// the status panel beside it.
#ifndef AUDI_STREAM_VIDEO_H
#define AUDI_STREAM_VIDEO_H

#include <array>
#include <cstdint>

#include "proto.h"
#include "vasm.h"

extern "C" {
#include "text.h"
}

namespace video {

constexpr int kTopbarH = 28;                  // status bar over a landscape picture
constexpr uint32_t kMaxBytes = 512u * 1024;   // one JPEG

struct View {
	canvas   frame{};     // the picture on screen, opaque ARGB
	bool     portrait = false;
	unsigned id = 0, shown = 0, bad = 0, dropped = 0;
	unsigned decode_ms = 0, present_ms = 0;   // present = render + copy + flip
	unsigned fps_x10 = 0; // pictures shown per 10 s
	unsigned bytes = 0;   // size of the picture on screen
	unsigned format = 0;  // its proto::kFmt*
};

// Screen size: sets the boxes in VIDEO_CAPS.
void init(int screen_w, int screen_h);
// VIDEO_CAPS payload.
std::array<uint8_t, proto::kVideoCapsLen> caps();

// usbd thread.
void submit(Picture pic);
// Streaming ended (VIDEO_STOP or link down): back to the status screen.
void stop(const char *why);

// Main loop: handle a stop and decode the newest waiting picture.
// true if what view() shows changed.
bool poll();
// Main loop: sleep until a picture or a stop is waiting, at most ms.
void wait(unsigned ms);

// How the screen with the new picture was presented (perf.h).
struct Present {
	unsigned render_us = 0, copy_us = 0, flip_us = 0, vfb_bytes = 0, vfb_rows = 0;
	bool     vfb_full = false;
};
// Main loop, after the screen with the new picture was presented; nullptr
// when headless. Acks it and records its timings.
void presented(const Present *p);
// nullptr when there is no picture (no streaming). Main loop only.
const View *view();

} // namespace video

#endif

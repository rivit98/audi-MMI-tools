// hwvideo.h - the phone's H.264 stream on the screen through the unit's
// DM642 decoder (common/video/hw_stream.h; protocol: proto.h H264_CONFIG, format
// kFmtH264, H264_STATUS). It runs its own thread ("hw"), which starts,
// polls and stops the stream.
//
//   usbd thread   hwvideo::config() (H264_CONFIG), hwvideo::frame() (a
//                 complete frame), hwvideo::stop() (VIDEO_STOP, link down)
//   manager       hwvideo::status_pop() -> H264_STATUS frames
//   main loop     hwvideo::active(): while true the screen belongs to the
//                 video; close the CPU-drawn screen, open it again after
//
// Writes "h264:" lines to the app's log: the config, the stream's start
// and end, refused frames, and every 10 s frames in / skips / io-media's
// reads and lag.
#ifndef AUDI_STREAM_HWVIDEO_H
#define AUDI_STREAM_HWVIDEO_H

#include <array>
#include "types.h"

#include "proto.h"

namespace hwvideo {

// The limits announced in VIDEO_CAPS (docs/phone-mirroring.md 7.10).
constexpr unsigned kMaxW = 720;
constexpr unsigned kMaxH = 576;
constexpr unsigned kFps = 25;
// Every frame is padded to the phone's frame size, and io-media reads it all:
// 256 KB frames (6.7 MB/s) are more than it reads in time, 128 KB ones keep up.
constexpr u32 kMaxFrame = 128u * 1024;

// VIDEO_CAPS payload: the screen as the landscape box, its left half as the
// portrait box (both kept on the wire, unused by the H.264 stream), only the
// H.264 format bit and the limits above.
std::array<u8, proto::kVideoCapsLen> caps(unsigned screen_w, unsigned screen_h);

void start();
void shutdown();

void config(const u8 *payload, unsigned n);
// One frame (Annex-B). Returns how it went, for VIDEO_ACKS.
proto::PicStatus frame(const u8 *data, unsigned size);
void stop(const char *why);

// Manager thread: next H264_STATUS code, true if there was one.
bool status_pop(u8 *code);
// Main loop: the video has (or is about to have) the screen.
bool active();

} // namespace hwvideo

#endif

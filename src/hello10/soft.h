/*
 * soft.h - hello10's sw test: how fast the SH4 decodes H.264 in software
 * (common/swdec.c, h264bsd) and how fast the picture reaches the screen.
 *
 * Three steps on the raw 800x480 Baseline clip (c12, Android Auto's size):
 *   decode  20 s, no picture: decode only, as fast as it goes
 *   bgra    15 s on screen: decode, BGRA on the CPU, gf_draw_image into
 *           the VFB (the 2D engine copies it), updateVfb
 *   yuv     10 s on screen: as bgra, but gf_draw_image gets YUV, so the 2D
 *           engine converts the colour: planar I420 if libgf takes it,
 *           else UYVY packed by the CPU (half of BGRA's bytes)
 * Every step logs per-frame times (decode, convert, draw, flip) and fps,
 * every 5 s and as a RESULT line, and adds a line to the summary.
 */
#ifndef HELLO10_SOFT_H
#define HELLO10_SOFT_H

#include "types.h"

#include <string>
#include <vector>

#include "h264.h"

namespace soft {

// data/frames: the clip, split into access units (frame 0 an IDR).
// Returns false if the run was asked to stop.
bool run(const u8 *data, const std::vector<h264::Range> &frames,
         std::vector<std::string> &summary);

} // namespace soft

#endif

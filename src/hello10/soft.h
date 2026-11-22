/*
 * soft.h - hello10's sw tests: how fast the SH4 decodes H.264 in software
 * (common/swdec.c, h264bsd), and every way we have of putting the decoded
 * picture on the screen, each as its own sub-test on the raw 800x480
 * Baseline clip (c12, Android Auto's size):
 *
 *   dec       decode only, as fast as it goes, no picture
 *   cpu       I420 -> BGRA by the CPU straight into the VFB (its CPU
 *             mapping), updateVfb: the path the text screens use
 *   image     BGRA by the CPU into RAM, gf_draw_image copies it into the VFB
 *   image565  as image, RGB565 (half the bytes; the 2D engine widens it)
 *   yuv       gf_draw_image of the decoder's planar I420 (the 2D engine
 *             converts the colour), else UYVY packed by the CPU
 *   blit      BGRA in RAM wrapped as a gf surface (gf_surface_attach),
 *             gf_draw_blit2 into the VFB: how layermanagerV2 itself copies a
 *             RAM picture into a surface (CImageLoader, P1001 0x08053ed8)
 *   gl        GL: RGBA texture upload (glFinish after it), textured quad,
 *             swap; GL draws on hardware layer 7, outside the layer manager
 *   gl565     as gl, an RGB565 texture (half the bytes)
 *
 * Every screen sub-test logs per-frame times (decode, convert, draw, flip),
 * fps every 5 s and a RESULT line, and checks every 25th picture by reading
 * sample pixels back from the surface it drew (readback.h), so the log says
 * whether the picture got there and not only how fast the loop ran. After
 * the timed part it saves the layer manager's state dump (which cid is on
 * layer 0, cid 40's sids) and, for the layer-manager paths, a d-shot of
 * cid 40 into the output directory.
 */
#ifndef HELLO10_SOFT_H
#define HELLO10_SOFT_H

#include "types.h"

#include <string>
#include <vector>

#include "h264.h"

namespace soft {

// The sub-test names above, in run order.
const std::vector<std::string> &all_methods();

// data/frames: the clip, split into access units (frame 0 an IDR).
// methods: comma-separated sub-test names (unknown ones are logged and
// skipped); out_dir: where the state dumps and d-shots go. Returns false if
// the run was asked to stop.
bool run(const u8 *data, const std::vector<h264::Range> &frames, const std::string &methods,
         const std::string &out_dir, std::vector<std::string> &summary);

} // namespace soft

#endif

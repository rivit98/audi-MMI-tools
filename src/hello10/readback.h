// readback.h - did the decoded picture land where we drew it? Pure: the
// caller reads pixels back from the surface (VFB vaddr, glReadPixels) and
// this compares them with what swdec_pixel_bgra says should be there, on a
// grid of sample points (tests/test_swdec.cpp).
#ifndef HELLO10_READBACK_H
#define HELLO10_READBACK_H

#include "types.h"

#include <cstdio>
#include <string>

#include "swdec.h"

namespace readback {

constexpr int kGrid = 4;   // kGrid x kGrid sample points

struct Result {
	int checks = 0;     // pictures checked
	int points = 0;     // pixels compared
	int bad = 0;        // pixels off by more than the tolerance
	int worst = 0;      // largest colour difference seen
	int x = -1, y = -1; // the first bad pixel
	u32 got = 0, want = 0;
};

// Sample i of n along a side of length len: cell centres, away from the edges.
inline int coord(int i, int len)
{
	return len * (2 * i + 1) / (2 * kGrid);
}

// get(x, y) returns the pixel at (x, y) of the picture's visible part as
// 0x..RRGGBB. tol: largest colour difference still counted as right (a
// format with fewer bits, or colour converted by the graphics chip).
template <class Get>
void check(const swdec_pic &p, int tol, Get get, Result &r)
{
	r.checks++;
	for (int j = 0; j < kGrid; j++)
		for (int i = 0; i < kGrid; i++) {
			int x = coord(i, p.crop_w), y = coord(j, p.crop_h);
			u32 want = swdec_pixel_bgra(&p, x, y), got = get(x, y);
			int d = swdec_rgb_diff(want, got);
			r.points++;
			r.worst = d > r.worst ? d : r.worst;
			if (d > tol) {
				if (r.bad == 0) {
					r.x = x;
					r.y = y;
					r.got = got;
					r.want = want;
				}
				r.bad++;
			}
		}
}

// "checks=2 points=32 bad=0 worst=3" + the first bad pixel, for the log.
inline std::string str(const Result &r)
{
	char b[160];
	int n = std::snprintf(b, sizeof(b), "checks=%d points=%d bad=%d worst=%d", r.checks, r.points,
	                      r.bad, r.worst);
	if (r.bad > 0 && n > 0 && (usize)n < sizeof(b))
		std::snprintf(b + n, sizeof(b) - (usize)n, " first_bad=(%d,%d) got=%06x want=%06x", r.x,
		              r.y, (unsigned)(r.got & 0xffffff), (unsigned)(r.want & 0xffffff));
	return b;
}

// What a check found, in a word: "ok" (every pixel right), "wrong" (none
// right), "partly" or "-" (never checked).
inline const char *verdict(const Result &r)
{
	if (r.points == 0)
		return "-";
	if (r.bad == 0)
		return "ok";
	return r.bad == r.points ? "wrong" : "partly";
}

} // namespace readback

#endif

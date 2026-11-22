// test_swdec.cpp - common/swdec.c (h264bsd) against ffmpeg, make test.
// tests/swdec_clips.sh makes the clips and ffmpeg's decode of them into
// build/test_swdec.d/ first. Every picture must come out of the access unit
// it was in (no frame of delay) and match ffmpeg byte for byte (H.264
// decoding is exact); swdec_to_bgra must match the BT.601 formula.
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "h264.h"
#include "swdec.h"

namespace {

std::vector<u8> slurp(const std::string &path)
{
	std::vector<u8> v;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return v;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	v.resize(n > 0 ? (usize)n : 0);
	if (std::fread(v.data(), 1, v.size(), f) != v.size())
		v.clear();
	std::fclose(f);
	return v;
}

u8 clip8(f64 v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (u8)(v + 0.5);
}

// The visible part of pic, in ffmpeg's rawvideo layout (planes w x h, w/2 x h/2).
std::vector<u8> visible(const swdec_pic &p)
{
	std::vector<u8> out;
	const u8 *planes[3] = {p.i420, p.i420 + p.width * p.height,
	                       p.i420 + p.width * p.height * 5 / 4};
	for (int k = 0; k < 3; k++) {
		int s = k ? 2 : 1, stride = p.width / s;
		for (int y = p.crop_top / s; y < (p.crop_top + p.crop_h) / s; y++) {
			const u8 *row = planes[k] + y * stride + p.crop_left / s;
			out.insert(out.end(), row, row + p.crop_w / s);
		}
	}
	return out;
}

// Largest difference between swdec_to_bgra and the formula, over the picture.
int bgra_error(const swdec_pic &p)
{
	std::vector<u32> px((usize)p.crop_w * p.crop_h);
	swdec_to_bgra(&p, px.data(), p.crop_w);
	std::vector<u8> v = visible(p);
	const u8 *Y = v.data(), *U = Y + p.crop_w * p.crop_h, *V = U + p.crop_w * p.crop_h / 4;
	int worst = 0;
	for (int y = 0; y < p.crop_h; y++)
		for (int x = 0; x < p.crop_w; x++) {
			f64 c = 1.164383 * (Y[y * p.crop_w + x] - 16);
			int ci = (y / 2) * (p.crop_w / 2) + x / 2;
			f64 d = U[ci] - 128, e = V[ci] - 128;
			u8 want[3] = {clip8(c + 2.017232 * d), clip8(c - 0.391762 * d - 0.812968 * e),
			              clip8(c + 1.596027 * e)};   // B, G, R
			u32 got = px[(usize)y * p.crop_w + x];
			if ((got >> 24) != 0xff)
				return 255;
			for (int k = 0; k < 3; k++) {
				int diff = std::abs((int)((got >> (8 * k)) & 0xff) - want[k]);
				worst = diff > worst ? diff : worst;
			}
		}
	return worst;
}

void check_clip(const std::string &dir, const char *name, int w, int h, int coded_w, int coded_h)
{
	std::vector<u8> es = slurp(dir + "/" + name + ".h264");
	std::vector<u8> ref = slurp(dir + "/" + name + ".yuv");
	CHECK(!es.empty() && !ref.empty());
	if (es.empty() || ref.empty())
		return;
	usize frame = (usize)w * h * 3 / 2;
	CHECK_EQ(ref.size() % frame, 0);

	swdec *d = swdec_open();
	CHECK(d != nullptr);
	std::vector<h264::Range> aus = h264::split_access_units(es.data(), es.size());
	usize pics = 0, mismatched = 0, late = 0, errors = 0;
	int worst_bgra = 0;
	for (const h264::Range &r : aus) {
		swdec_pic p;
		std::memset(&p, 0, sizeof(p));
		int rc = swdec_decode(d, es.data() + r.begin, r.end - r.begin, &p);
		if (rc != 1) {
			late++;
			continue;
		}
		errors += p.error_mbs != 0;
		if (pics == 0) {
			CHECK_EQ(p.width, coded_w);
			CHECK_EQ(p.height, coded_h);
			CHECK_EQ(p.crop_w, w);
			CHECK_EQ(p.crop_h, h);
			CHECK_EQ(p.idr, 1);
			worst_bgra = bgra_error(p);
		}
		std::vector<u8> v = visible(p);
		if ((pics + 1) * frame > ref.size() || std::memcmp(v.data(), ref.data() + pics * frame, frame))
			mismatched++;
		pics++;
	}
	swdec_close(d);
	std::printf("test_swdec: %s: %zu access units, %zu pictures, %zu differ from ffmpeg, "
	            "BGRA off by %d at most\n",
	            name, aus.size(), pics, mismatched, worst_bgra);
	CHECK_EQ(pics, ref.size() / frame);
	CHECK_EQ(late, 0);
	CHECK_EQ(mismatched, 0);
	CHECK_EQ(errors, 0);
	CHECK(worst_bgra <= 1);
}

} // namespace

int main(int argc, char *argv[])
{
	std::string dir = argc > 1 ? argv[1] : "build/test_swdec.d";
	check_clip(dir, "full", 800, 480, 800, 480);
	check_clip(dir, "crop", 360, 200, 368, 208);

	// garbage in: no crash, no picture
	swdec *d = swdec_open();
	const u8 junk[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0xff, 0x13};
	swdec_pic p;
	CHECK(swdec_decode(d, junk, sizeof(junk), &p) != 1);
	swdec_close(d);
	CHECK_DONE();
}

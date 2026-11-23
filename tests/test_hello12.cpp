// test_hello12.cpp - hello12's SBC header parser, frame index, live release
// and read answers (src/hello12/sbc.h).
#include "check.h"
#include "sbc.h"
#include "types.h"

#include <string>
#include <vector>

int main()
{
	// ffmpeg's SBC encoder at 328 kbit/s (media/mkclips.sh's a1/a2):
	// 9C B9 36 = 44.1 kHz, 16 blocks, stereo, loudness, 8 subbands,
	// bitpool 54 -> 4 + 8 + 16 * 54 / 8 = 120 bytes (20 s = 826,800 bytes)
	const u8 ff[4] = {0x9C, 0xB9, 0x36, 0x02};
	sbc::Header h;
	CHECK(sbc::parse(ff, 4, h));
	CHECK_EQ(h.rate, 44100);
	CHECK_EQ(h.blocks, 16);
	CHECK_EQ(h.mode, sbc::kStereo);
	CHECK_EQ(h.loudness, 1);
	CHECK_EQ(h.subbands, 8);
	CHECK_EQ(h.bitpool, 54);
	CHECK_EQ(h.channels, 2);
	CHECK_EQ(h.samples, 128);
	CHECK_EQ(h.frame_len, 120);

	// A2DP's "high quality" settings: joint stereo bitpool 53 = 119 bytes,
	// 48 kHz bitpool 51 = 115 (A2DP spec table 4.7)
	CHECK_EQ(sbc::frame_len(sbc::kJoint, 8, 16, 53), 119);
	CHECK_EQ(sbc::frame_len(sbc::kJoint, 8, 16, 51), 115);
	// mono, 4 subbands, 8 blocks, bitpool 10: 4 + 2 + 10 = 16
	CHECK_EQ(sbc::frame_len(sbc::kMono, 4, 8, 10), 16);
	// dual: 4 + 8 + ceil(16 * 2 * 20 / 8) = 92; stereo: 4 + 8 + 40 = 52
	CHECK_EQ(sbc::frame_len(sbc::kDual, 8, 16, 20), 92);
	CHECK_EQ(sbc::frame_len(sbc::kStereo, 8, 16, 20), 52);

	const u8 bad_sync[4] = {0x9D, 0xB9, 0x36, 0};
	CHECK(!sbc::parse(bad_sync, 4, h));
	const u8 bad_pool[4] = {0x9C, 0xB9, 0x01, 0};
	CHECK(!sbc::parse(bad_pool, 4, h));
	CHECK(!sbc::parse(ff, 3, h));
	// D2 = 48 kHz, 8 blocks, mono, SNR, 4 subbands; bitpool 10
	const u8 mono[4] = {0x9C, 0xD2, 10, 0};
	CHECK(sbc::parse(mono, 4, h));
	CHECK_EQ(h.rate, 48000);
	CHECK_EQ(h.blocks, 8);
	CHECK_EQ(h.mode, sbc::kMono);
	CHECK_EQ(h.loudness, 0);
	CHECK_EQ(h.subbands, 4);

	// index: three frames, then a cut-off one
	std::vector<u8> clip;
	for (int i = 0; i < 3; i++) {
		usize at = clip.size();
		clip.resize(at + 120);
		clip[at] = 0x9C;
		clip[at + 1] = 0xB9;
		clip[at + 2] = 0x36;
	}
	std::vector<u32> offs;
	sbc::Header first;
	std::string err = sbc::index(clip.data(), clip.size(), offs, &first);
	CHECK_STR(err.c_str(), "");
	CHECK_EQ(offs.size(), 3);
	CHECK_EQ(offs[2], 240);
	CHECK_EQ(first.frame_len, 120);
	CHECK(sbc::index(clip.data(), clip.size() - 1, offs, nullptr) == "frame 2 at byte 240 is cut off");
	clip[120] = 0;
	CHECK(sbc::index(clip.data(), clip.size(), offs, nullptr) == "no SBC frame header at byte 120");
	clip[120] = 0x9C;
	clip[121] = 0xBD;   // joint stereo: 121 bytes
	CHECK(sbc::index(clip.data(), clip.size(), offs, nullptr).find("changes format") != std::string::npos);
	CHECK(sbc::index(clip.data(), 0, offs, nullptr) == "empty");

	// release: 128 samples at 44.1 kHz = 2902.5 us per frame
	CHECK_EQ(sbc::released(1000, 128, 44100, 0, 0), 1);
	CHECK_EQ(sbc::released(1000, 128, 44100, 0, 2902), 1);
	CHECK_EQ(sbc::released(1000, 128, 44100, 0, 2903), 2);
	CHECK_EQ(sbc::released(1000, 128, 44100, 69, 0), 70);
	CHECK_EQ(sbc::released(1000, 128, 44100, 0, 1000000), 345);   // 344.5 frames/s
	CHECK_EQ(sbc::released(1000, 128, 44100, 0, 3600000000ull), 1000);
	CHECK_EQ(sbc::released(7, 0, 44100, 0, 0), 7);

	// answers: total 1000 bytes, 400 out
	u32 n = 99;
	CHECK(!sbc::answer(0, 512, 400, 1000, false, false, &n));     // whole range not out: hold
	CHECK(sbc::answer(0, 512, 400, 1000, false, true, &n));       // partial: what is there
	CHECK_EQ(n, 400);
	CHECK(sbc::answer(0, 300, 400, 1000, false, false, &n));
	CHECK_EQ(n, 300);
	CHECK(!sbc::answer(400, 100, 400, 1000, false, true, &n));    // nothing past off yet
	CHECK(sbc::answer(900, 512, 1000, 1000, true, false, &n));    // the end: what is left
	CHECK_EQ(n, 100);
	CHECK(sbc::answer(1000, 512, 1000, 1000, true, false, &n));   // EOF
	CHECK_EQ(n, 0);
	CHECK(sbc::answer(1200, 512, 400, 1000, false, false, &n));   // past the end: EOF
	CHECK_EQ(n, 0);

	printf("test_hello12: %d checks, %d failed\n", check_count, check_failures);
	return check_failures != 0;
}

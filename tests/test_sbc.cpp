// test_sbc.cpp - the SBC parser and CRC (src/common/sbc.h) against frames
// from BlueZ's encoder (third_party/sbc-*).
#include "check.h"
#include "sbc.h"
#include "types.h"

extern "C" {
#include "sbc/sbc.h"
}

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

// n frames of a stereo tone (or silence) from libsbc with these settings.
std::vector<u8> encode(int mode, int alloc, int subbands, int blocks, int bitpool, int nframes,
                       bool quiet, usize *flen)
{
	sbc_t s;
	sbc_init(&s, 0);
	s.frequency = SBC_FREQ_44100;
	s.mode = (u8)mode;
	s.allocation = (u8)alloc;
	s.subbands = (u8)subbands;
	s.blocks = (u8)blocks;
	s.bitpool = (u8)bitpool;
	s.endian = SBC_LE;
	usize code = sbc_get_codesize(&s);
	*flen = sbc_get_frame_length(&s);
	int ch = mode == SBC_MODE_MONO ? 1 : 2;
	std::vector<i16> pcm(code / 2);
	std::vector<u8> out;
	u8 frame[512];
	usize t = 0;
	for (int f = 0; f < nframes; f++) {
		for (usize i = 0; i < pcm.size(); i += (usize)ch, t++) {
			f64 v = quiet ? 0 : 8000 * std::sin(2 * M_PI * 1000 * (f64)t / 44100);
			for (int c = 0; c < ch; c++)
				pcm[i + (usize)c] = (i16)(c ? v / 2 : v);
		}
		ssize_t written = 0;   // types-ok: libsbc's API
		ssize_t used = sbc_encode(&s, pcm.data(), code, frame, sizeof(frame), &written); // types-ok
		CHECK_EQ(used, (i64)code);
		CHECK_EQ(written, (i64)*flen);
		out.insert(out.end(), frame, frame + written);
	}
	sbc_finish(&s);
	return out;
}

} // namespace

int main()
{
	// Parser + CRC on every frame of every mode and both allocations.
	struct { int mode, alloc, subbands, blocks, bitpool; } kinds[] = {
		{SBC_MODE_JOINT_STEREO, SBC_AM_LOUDNESS, SBC_SB_8, SBC_BLK_16, 53},  // A2DP high quality
		{SBC_MODE_STEREO, SBC_AM_LOUDNESS, SBC_SB_8, SBC_BLK_16, 54},        // the hello12 clips
		{SBC_MODE_DUAL_CHANNEL, SBC_AM_SNR, SBC_SB_4, SBC_BLK_8, 20},
		{SBC_MODE_MONO, SBC_AM_SNR, SBC_SB_8, SBC_BLK_12, 31},
	};
	for (const auto &k : kinds) {
		usize flen = 0;
		std::vector<u8> d = encode(k.mode, k.alloc, k.subbands, k.blocks, k.bitpool, 50, false, &flen);
		unsigned ok = 0;
		for (usize o = 0; o + flen <= d.size(); o += flen) {
			sbc::Header h;
			if (sbc::frame_ok(d.data() + o, flen, &h) && h.frame_len == flen)
				ok++;
		}
		CHECK_EQ(ok, 50);
		sbc::Header h;
		CHECK(sbc::parse(d.data(), flen, h));
		CHECK_EQ(h.loudness, k.alloc == SBC_AM_LOUDNESS ? 1 : 0);
		CHECK_EQ(h.bitpool, k.bitpool);
		// a flipped scale-factor bit, a flipped bitpool: the CRC no longer fits
		std::vector<u8> bad(d.begin(), d.begin() + (long)flen);
		bad[4] ^= 0x10;
		CHECK(!sbc::frame_ok(bad.data(), flen));
		bad[4] ^= 0x10;
		CHECK(sbc::frame_ok(bad.data(), flen));
		CHECK(!sbc::frame_ok(bad.data(), flen - 1));   // cut short
	}

	printf("test_sbc: %d checks, %d failed\n", check_count, check_failures);
	return check_failures != 0;
}

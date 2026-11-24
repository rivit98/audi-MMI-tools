// test_audio_rx.cpp - the SBC parser and CRC (src/common/sbc.h) against
// frames from BlueZ's encoder (third_party/sbc-*, the one the phone app
// uses), the AUDIO_CONFIG wire format (proto.h), and the receiver's
// stream checks (src/audi_stream_receiver/audio_rx.h).
#include "check.h"
#include "audio_rx.h"
#include "proto.h"
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

// AUDIO_DATA: seq, phone_ms, frames [from, from + n) of data.
std::vector<u8> msg(u32 seq, u32 phone_ms, const std::vector<u8> &data, usize flen, usize from,
                    usize n)
{
	std::vector<u8> m(proto::kAudioDataHdr);
	proto::put_u32(m.data(), seq);
	proto::put_u32(m.data() + 4, phone_ms);
	m.insert(m.end(), data.begin() + (long)(from * flen), data.begin() + (long)((from + n) * flen));
	return m;
}

bool has(const std::string &s, const char *part)
{
	bool ok = s.find(part) != std::string::npos;
	if (!ok)
		fprintf(stderr, "  missing \"%s\" in: %s\n", part, s.c_str());
	return ok;
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

	// AUDIO_CONFIG: round trip, and what is refused
	proto::AudioConfig c;
	c.codec = proto::kAudioCodecSbc;
	c.rate = 44100;
	c.channels = 2;
	c.blocks = 16;
	c.subbands = 8;
	c.mode = 3;
	c.loudness = 1;
	c.bitpool = 53;
	c.frame_len = 119;
	u8 wire[proto::kAudioConfigLen];
	proto::encode_audio_config(c, wire);
	auto back = proto::parse_audio_config(wire, sizeof(wire));
	CHECK(back.has_value());
	CHECK_EQ(back->rate, 44100);
	CHECK_EQ(back->frame_len, 119);
	CHECK_EQ(back->mode, 3);
	CHECK(!proto::parse_audio_config(wire, sizeof(wire) - 1));
	wire[0] = 2;                                          // not SBC
	CHECK(!proto::parse_audio_config(wire, sizeof(wire)));
	wire[0] = 1;
	wire[5] = 1;                                          // 1 channel but joint
	CHECK(!proto::parse_audio_config(wire, sizeof(wire)));
	wire[5] = 2;

	// The receiver: 100 good frames in 10 messages, one gap, one bad frame.
	usize flen = 0;
	std::vector<u8> d = encode(SBC_MODE_JOINT_STEREO, SBC_AM_LOUDNESS, SBC_SB_8, SBC_BLK_16, 53, 120,
	                           false, &flen);
	CHECK_EQ(flen, 119);
	audio::Rx rx(119 * 30);
	std::string ended;
	CHECK(rx.config(wire, sizeof(wire), 1000, &ended).empty());
	CHECK(ended.empty());
	CHECK(rx.active());
	// 10 frames (~29 ms) per message, the MMI 5 ms later each time, then
	// 20 ms later for the last ones: lag 0 .. 15
	for (u32 m = 0; m < 10; m++) {
		u32 seq = m * 10 + (m >= 5 ? 10 : 0);              // message 5 skips 10 frames
		std::vector<u8> one = msg(seq, 50000 + seq * 29, d, flen, seq, 10);
		if (m == 7)
			one[proto::kAudioDataHdr + 5] ^= 0xFF;         // its first frame's CRC breaks
		rx.data(one.data(), (unsigned)one.size(), 1000 + seq * 29 + (m >= 8 ? 15 : 0));
	}
	CHECK(rx.window(5000).empty());                        // not 10 s yet
	std::string w = rx.window(11000);
	CHECK(has(w, "99 frames"));
	CHECK(has(w, "1 bad"));
	CHECK(has(w, "10 missing"));
	CHECK(has(w, "10 msgs"));
	CHECK(has(w, "lag min/max/last 0/15/15 ms"));
	CHECK_EQ(rx.totals().frames, 99);

	// a short message, a frame cut short, a seq going back
	u8 tiny[3] = {0, 0, 0};
	rx.data(tiny, 3, 11100);
	std::vector<u8> cut = msg(110, 53190, d, flen, 110, 2);
	cut.pop_back();
	rx.data(cut.data(), (unsigned)cut.size(), 11200);
	std::vector<u8> again = msg(0, 53200, d, flen, 0, 1);
	rx.data(again.data(), (unsigned)again.size(), 11210);
	std::string e = rx.end("AUDIO_STOP", 12000);
	CHECK(has(e, "stream ended (AUDIO_STOP)"));
	CHECK(has(e, "1 short"));
	CHECK(has(e, "1 back"));
	CHECK_EQ(rx.totals().frames, 99 + 1 + 1);
	CHECK_EQ(rx.totals().bad, 2);
	CHECK(!rx.active());
	rx.data(again.data(), (unsigned)again.size(), 12100);  // no config: ignored
	CHECK_EQ(rx.totals().msgs, 12);

	// The capture: the first 30 frames (the cap, 30 x 119), once
	std::vector<u8> cap = rx.take_capture();
	CHECK_EQ(cap.size(), 30 * 119);
	CHECK(cap.size() >= 119 && std::memcmp(cap.data(), d.data(), 119) == 0);
	CHECK(rx.take_capture().empty());

	// silence is counted; a new config ends the running stream
	std::vector<u8> q = encode(SBC_MODE_JOINT_STEREO, SBC_AM_LOUDNESS, SBC_SB_8, SBC_BLK_16, 53, 5,
	                           true, &flen);
	CHECK(rx.config(wire, sizeof(wire), 20000, &ended).empty());
	std::vector<u8> qm = msg(0, 1, q, flen, 0, 5);
	rx.data(qm.data(), (unsigned)qm.size(), 20010);
	CHECK(rx.config(wire, sizeof(wire), 20020, &ended).empty());
	CHECK(has(ended, "a new AUDIO_CONFIG"));
	CHECK(has(ended, "5 silent"));
	// a config whose frame_len doesn't match its fields is refused
	proto::put_u16(wire + 11, 120);
	CHECK(!rx.config(wire, sizeof(wire), 20030, &ended).empty());
	CHECK(!rx.active());

	printf("test_audio_rx: %d checks, %d failed\n", check_count, check_failures);
	return check_failures != 0;
}

// wms.h - what a stand-in for the Bluetooth stack's /dev/wms/player1 must
// answer (docs/bt-audio-path.md §4-5), and the looped byte stream it serves
// (pure: tests/test_hello12.cpp).
//
// io-media's a2dp_player graph, before it plays (play_prep), opens the path
// and sends two devctls; on stop a third:
//   0x4004051e  4 bytes out: non-zero = "bt connected"
//   0x40100520  16 bytes out: {i32 state (1 = streaming), i32 codec
//               (0 PCM, 1 SBC, 2 MPG, 3 AAC, 4 ATC), i32 rate (0 = 44100,
//               1 = 48000), i32 unused}
//   0x532       no data: end of stream
// MediaRMService's devctl switch also knows 0x40040525, 0x40010522 and
// 0x400c0520, whose meaning is unknown: they, and anything else, get
// ENOSYS, and the log says who asked.
#ifndef HELLO12_WMS_H
#define HELLO12_WMS_H

#include "types.h"

namespace wms {

constexpr u32 kDcmdConnected = 0x4004051eu;
constexpr u32 kDcmdStreamInfo = 0x40100520u;
constexpr u32 kDcmdEof = 0x532u;

constexpr i32 kCodecPcm = 0;
constexpr i32 kCodecSbc = 1;
constexpr i32 kRate44100 = 0;
constexpr i32 kRate48000 = 1;

struct Answer {
	bool handled = false;  // false: ENOSYS
	u8 out[16] = {};       // the reply data (little endian i32s)
	u32 len = 0;           // bytes of out to send (never more than asked)
	const char *name = "unknown";
};

// The answer to devctl dcmd with room for nbytes of reply data, for a
// stream of codec / rate (kCodec*, kRate*) that is streaming.
Answer devctl(u32 dcmd, u32 nbytes, i32 codec, i32 rate);

// Copy n bytes of the endless stream "data repeated" from offset off.
void copy_looped(const u8 *data, usize size, u64 off, u8 *out, usize n);

} // namespace wms

#endif

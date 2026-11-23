// sbc.h - SBC frame headers, a clip's frame index, and the real-time
// release of a live SBC stream (pure: tests/test_hello12.cpp).
//
// SBC frame header (A2DP spec 12.6): sync 0x9C; byte 1 = sampling frequency
// (2 bits: 16, 32, 44.1, 48 kHz), blocks (2 bits: 4, 8, 12, 16), channel
// mode (2 bits: mono, dual, stereo, joint), allocation (1 bit), subbands
// (1 bit: 4, 8); byte 2 = bitpool; byte 3 = CRC.
#ifndef HELLO12_SBC_H
#define HELLO12_SBC_H

#include "types.h"

#include <string>
#include <vector>

namespace sbc {

enum Mode : u8 { kMono = 0, kDual = 1, kStereo = 2, kJoint = 3 };

struct Header {
	u32 rate = 0;        // Hz
	u8 blocks = 0;       // 4, 8, 12, 16
	u8 mode = 0;         // Mode
	u8 loudness = 0;     // allocation: 1 = loudness, 0 = SNR
	u8 subbands = 0;     // 4, 8
	u8 bitpool = 0;
	u8 channels = 0;     // 1 or 2
	usize frame_len = 0; // bytes, header included
	u32 samples = 0;     // per channel per frame = blocks * subbands
};

// Parse the header at p (n bytes available). False: no sync byte, too
// short, or a frame length that cannot be.
bool parse(const u8 *p, usize n, Header &h);

// Frame length in bytes from the header fields (A2DP spec 12.9).
usize frame_len(u8 mode, u8 subbands, u8 blocks, u8 bitpool);

// Every frame's start in data. "" or why the data is not a plain run of
// SBC frames with one format (the first header in *first).
std::string index(const u8 *data, usize n, std::vector<u32> &offs, Header *first);

// A live stream released in real time: frame k is there prebuffer frames
// early, i.e. at (k - prebuffer) * frame duration after the first read.
// Returns how many frames are out at elapsed_us (never more than count).
usize released(usize count, u32 samples, u32 rate, usize prebuffer, u64 elapsed_us);

// How to answer a read of nbytes at off, with avail bytes out of total.
// partial: answer as soon as anything past off is out (a live source's
// short read); else only when the whole range is out (or the stream is
// complete). Returns false to hold the read, true with *n bytes to send
// (0 = end of stream).
bool answer(u64 off, u32 nbytes, u64 avail, u64 total, bool complete, bool partial, u32 *n);

} // namespace sbc

#endif

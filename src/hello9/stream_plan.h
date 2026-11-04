// stream_plan.h - the pure part of hello9's live stream (no QNX calls, so
// it is tested on the build host: tests/test_hello9.cpp).
//
// The stream is a constant-sample-size MP4 (media/csmp4.py,
// docs/phone-mirroring.md 7.8): a moov with one-entry tables, then mdat
// with an 8-byte header (size 0 or exact; 7.9), then frame k at
// data_off + k * sample_size. The header may declare more frames than
// the file holds; the server loops the frames it has.
#ifndef HELLO9_STREAM_PLAN_H
#define HELLO9_STREAM_PLAN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace plan {

struct CsMp4 {
	uint64_t data_off = 0;      // frame 0's first byte (stco[0])
	uint32_t sample_size = 0;   // S, stsz's constant size
	uint32_t declared = 0;      // frames the header declares (stsz count)
	uint32_t present = 0;       // whole frames in the file
	uint32_t timescale = 0;     // mdhd
	uint32_t delta = 0;         // stts' one sample duration

	uint64_t total() const { return data_off + (uint64_t)declared * sample_size; }
	double fps() const { return delta ? (double)timescale / delta : 0; }
};

// Parse the start of a CS-MP4 (head[0..len), at least ftyp + moov) of a file
// file_size bytes long. "" on success, else what is wrong.
std::string parse(const uint8_t *head, size_t len, uint64_t file_size, CsMp4 &out);

// A pause in the stream: frames from at_frame on come ms later.
struct Stall {
	uint32_t at_frame;
	uint32_t ms;
};

// When each frame becomes available, in ms after the stream's start.
class Schedule {
public:
	Schedule(double fps, std::vector<Stall> stalls);
	uint64_t release_ms(uint64_t frame) const;
	// Frames released at elapsed_ms (frame 0 is released at 0).
	uint64_t released(uint64_t elapsed_ms) const;
	// Sum of the stalls before frame (i.e. at_frame <= frame).
	uint64_t stall_ms_before(uint64_t frame) const;

private:
	double fps_;
	std::vector<Stall> stalls_;
};

// Where served byte off comes from in the source file, and how many bytes
// from there on are contiguous (to the end of the header or the frame).
struct Span {
	uint64_t src_off;
	uint64_t len;
};
Span map(const CsMp4 &m, uint64_t off);

// Frames that must be released before bytes [0, end) can be served.
uint64_t frames_needed(const CsMp4 &m, uint64_t end);

} // namespace plan

#endif

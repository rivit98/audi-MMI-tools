// schedule.h - when hello9's feeder hands each frame of a test clip to the
// live stream, the way a phone's frames would arrive: at a steady rate, with
// pauses, or in bursts. Pure C++ (tests/test_live.cpp runs it on the build
// host).
#ifndef HELLO9_SCHEDULE_H
#define HELLO9_SCHEDULE_H

#include <cstdint>
#include <vector>

namespace plan {

// A pause in the stream: frames from at_frame on come ms later.
struct Stall {
	uint32_t at_frame;
	uint32_t ms;
};

// When each frame becomes available, in ms after the stream's start.
class Schedule {
public:
	// fps: the rate frames come at. burst > 1: they come burst at a time,
	// each group when its first frame is due.
	Schedule(double fps, std::vector<Stall> stalls, unsigned burst = 1);
	uint64_t release_ms(uint64_t frame) const;
	// Frames released at elapsed_ms (frame 0 is released at 0).
	uint64_t released(uint64_t elapsed_ms) const;
	// Sum of the stalls before frame (i.e. at_frame <= frame).
	uint64_t stall_ms_before(uint64_t frame) const;
	double fps() const { return fps_; }

private:
	double fps_;
	std::vector<Stall> stalls_;
	unsigned burst_;
};

} // namespace plan

#endif

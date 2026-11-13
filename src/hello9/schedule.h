// schedule.h - when hello9's feeder hands each frame of a test clip to the
// live stream, the way a phone's frames would arrive: at a steady rate, with
// pauses, or in bursts. Pure C++ (tests/test_live.cpp runs it on the build
// host).
#ifndef HELLO9_SCHEDULE_H
#define HELLO9_SCHEDULE_H

#include "types.h"
#include <vector>

namespace plan {

// A pause in the stream: frames from at_frame on come ms later.
struct Stall {
	u32 at_frame;
	u32 ms;
};

// When each frame becomes available, in ms after the stream's start.
class Schedule {
public:
	// fps: the rate frames come at. burst > 1: they come burst at a time,
	// each group when its first frame is due.
	Schedule(f64 fps, std::vector<Stall> stalls, unsigned burst = 1);
	u64 release_ms(u64 frame) const;
	// Frames released at elapsed_ms (frame 0 is released at 0).
	u64 released(u64 elapsed_ms) const;
	// Sum of the stalls before frame (i.e. at_frame <= frame).
	u64 stall_ms_before(u64 frame) const;
	f64 fps() const { return fps_; }

private:
	f64 fps_;
	std::vector<Stall> stalls_;
	unsigned burst_;
};

} // namespace plan

#endif

// schedule.cpp - see schedule.h.
#include "schedule.h"

#include <utility>

namespace plan {

Schedule::Schedule(double fps, std::vector<Stall> stalls, unsigned burst)
	: fps_(fps), stalls_(std::move(stalls)), burst_(burst ? burst : 1)
{
}

uint64_t Schedule::stall_ms_before(uint64_t frame) const
{
	uint64_t ms = 0;

	for (const Stall &s : stalls_)
		if (s.at_frame <= frame)
			ms += s.ms;
	return ms;
}

uint64_t Schedule::release_ms(uint64_t frame) const
{
	uint64_t lead = frame - frame % burst_;      // the group's first frame
	return (uint64_t)((double)lead * 1000.0 / fps_ + 1e-6) + stall_ms_before(frame);
}

uint64_t Schedule::released(uint64_t elapsed_ms) const
{
	// release_ms is monotonic: the first frame not yet released, by bisection
	uint64_t lo = 0, hi = (uint64_t)((double)elapsed_ms * fps_ / 1000.0) + burst_ + 2;

	while (lo < hi) {
		uint64_t mid = lo + (hi - lo) / 2;
		if (release_ms(mid) <= elapsed_ms)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

} // namespace plan

// schedule.cpp - see schedule.h.
#include "schedule.h"
#include "types.h"

#include <utility>

namespace plan {

Schedule::Schedule(f64 fps, std::vector<Stall> stalls, unsigned burst)
	: fps_(fps), stalls_(std::move(stalls)), burst_(burst ? burst : 1)
{
}

u64 Schedule::stall_ms_before(u64 frame) const
{
	u64 ms = 0;

	for (const Stall &s : stalls_)
		if (s.at_frame <= frame)
			ms += s.ms;
	return ms;
}

u64 Schedule::release_ms(u64 frame) const
{
	u64 lead = frame - frame % burst_;      // the group's first frame
	return (u64)((f64)lead * 1000.0 / fps_ + 1e-6) + stall_ms_before(frame);
}

u64 Schedule::released(u64 elapsed_ms) const
{
	// release_ms is monotonic: the first frame not yet released, by bisection
	u64 lo = 0, hi = (u64)((f64)elapsed_ms * fps_ / 1000.0) + burst_ + 2;

	while (lo < hi) {
		u64 mid = lo + (hi - lo) / 2;
		if (release_ms(mid) <= elapsed_ms)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

} // namespace plan

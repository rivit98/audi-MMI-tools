// rate_limit.h - a warning logged at most once per period, with a count of
// the ones held back meanwhile. Pure (no locking: one thread per limiter, the
// time passed in), tests/test_pacer.cpp.
//
//   static RateLimit warn(10000);
//   unsigned held;
//   if (warn.allow(log_ms(), &held))
//       log_msg("...: lag over 1 s (%u more not logged)", held);
#ifndef COMMON_RATE_LIMIT_H
#define COMMON_RATE_LIMIT_H

class RateLimit {
public:
	explicit RateLimit(unsigned period_ms) : period_ms_(period_ms) {}

	// true: log it now; *suppressed = how many were held back since the
	// last one logged. false: counted, not logged. now_ms may wrap.
	bool allow(unsigned now_ms, unsigned *suppressed = nullptr)
	{
		if (logged_ && now_ms - last_ms_ < period_ms_) {
			held_++;
			return false;
		}
		if (suppressed != nullptr)
			*suppressed = held_;
		held_ = 0;
		logged_ = true;
		last_ms_ = now_ms;
		return true;
	}

	// Held back since the last one logged.
	unsigned held() const { return held_; }

private:
	unsigned period_ms_;
	unsigned last_ms_ = 0, held_ = 0;
	bool logged_ = false;
};

#endif

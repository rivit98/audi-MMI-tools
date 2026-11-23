// state.h - is the rear camera up: the two signals combined. Pure,
// tests/test_camera.cpp.
//
//   vcap   the HMI owns VCAP 0 (slog, vcap.h). The authority: while it holds,
//          the camera is up.
//   bap    the camera's control unit announced a session (ch4, bap.h), about
//          1 s before the HMI takes VCAP 0: the early warning.
//
// active() = vcap || bap. BAP is only sent on a change, and ch4 telegrams can
// get lost, so a BAP "on" is dropped when
//   - the HMI lets VCAP 0 go after taking it (a lost "off"), or
//   - the HMI hasn't taken VCAP 0 kBapAloneMs after it (an "on" nobody
//     followed; unconfirmed() counts them).
// A lost "on" costs only the early warning: the slog still sees the camera.
#ifndef CAMERA_STATE_H
#define CAMERA_STATE_H

#include "types.h"

namespace camera {

// After the camera's capture goes off, the HMI still has to show its next
// context. A show of ours before that would be replaced.
constexpr unsigned kSettleMs = 1000;
// A BAP "on" the HMI hasn't followed by then is dropped (on the car it takes
// 0.9-1.3 s).
constexpr unsigned kBapAloneMs = 5000;

class Combiner {
public:
	// Each returns true when active() changed.
	bool vcap(bool owned, unsigned ms)
	{
		bool was = active();
		if (owned) {
			vcap_ = true;
			confirmed_ = bap_;
		} else {
			vcap_ = false;
			if (bap_ && confirmed_)
				bap_ = false;           // the session is over, whatever BAP said
		}
		return changed(was, ms);
	}

	bool bap(bool on, unsigned ms)
	{
		bool was = active();
		if (on && !bap_) {
			bap_ = true;
			bap_ms_ = ms;
			confirmed_ = vcap_;
		} else if (!on) {
			bap_ = false;
		}
		return changed(was, ms);
	}

	// Call regularly: drops an unconfirmed BAP "on".
	bool tick(unsigned ms)
	{
		bool was = active();
		if (bap_ && !confirmed_ && !vcap_ && ms - bap_ms_ >= kBapAloneMs) {
			bap_ = false;
			unconfirmed_++;
		}
		return changed(was, ms);
	}

	bool active() const { return vcap_ || bap_; }
	bool vcap_owned() const { return vcap_; }
	bool bap_on() const { return bap_; }
	// Not active, and kSettleMs since it last ended (or it never was).
	bool clear(unsigned now_ms) const
	{
		return !active() && (!ended_ || now_ms - ended_ms_ >= kSettleMs);
	}
	bool ended() const { return ended_; }
	unsigned ended_ms() const { return ended_ms_; }
	u32 unconfirmed() const { return unconfirmed_; }

private:
	bool changed(bool was, unsigned ms)
	{
		if (was && !active()) {
			ended_ = true;
			ended_ms_ = ms;
		}
		return was != active();
	}

	bool vcap_ = false, bap_ = false, confirmed_ = false, ended_ = false;
	unsigned bap_ms_ = 0, ended_ms_ = 0;
	u32 unconfirmed_ = 0;
};

} // namespace camera

#endif

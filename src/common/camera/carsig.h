// carsig.h - the car's own signals on /dev/ipc/ch4 (docs/vehicle-data.md
// §5.1-5.2), as log lines: every non-BAP function's changes, at most one
// line per function per min_gap_ms (the changes in between are counted and
// the latest value logged when the gap has passed; the light functions
// 0x0C0C / 0x0C0D every change), and the night state.
// Pure (no locking, the time passed in), tests/test_carsig.cpp.
//
// Night: the HMI's night map follows DA01.0C0D's first byte (non-zero =
// night; on the car 0 with the lights off and 100 with them on, likely the
// instrument dimming in percent). It is sent only when it changes, so it is
// unknown until the first telegram after the start.
#ifndef CAMERA_CARSIG_H
#define CAMERA_CARSIG_H

#include "ipc4.h"
#include "types.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace carsig {

constexpr u16 kDimmingFct = ipc4::kInteriorLightFct;   // 0x0C0D

// What a function is (vehicle-data.md §5.1 and the firmware's dispatch), or "".
inline const char *name(const ipc4::Most &m)
{
	if (m.fblock == ipc4::kCarFblock) {
		switch (m.fct) {
		case 0x0C0A: return "clamp/energy";
		case 0x0C0C: return "exterior light";
		case 0x0C0D: return "dimming (night)";
		case 0x0C0E: return "climate/sound";
		case 0x0C10: return "driving state";
		case 0x0C13: return "trailer";
		case 0x0C36: return "crash";
		default: return "";
		}
	}
	if (m.fblock == 0xDD)
		return "cluster";
	return "";
}

enum class Night { Unknown, Day, Night };

inline const char *night_name(Night n)
{
	return n == Night::Night ? "NIGHT" : n == Night::Day ? "day" : "unknown";
}

class Logger {
public:
	explicit Logger(unsigned min_gap_ms = 5000) : gap_ms_(min_gap_ms) {}

	// One telegram's function. BAP streams are left out (the clock and a
	// counter change every second). Lines for the log go to out.
	void feed(unsigned ms, const ipc4::Most &m, std::vector<std::string> &out)
	{
		if (ipc4::is_bap(m))
			return;
		std::string key = ipc4::most_key(m);
		const char *nm = name(m);
		if (*nm)
			key += std::string(" ") + nm;
		Key &k = keys_[key];
		if (m.fblock == ipc4::kCarFblock && m.fct == kDimmingFct && !m.payload.empty()) {
			dimming_ = m.payload[0];
			Night n = dimming_ != 0 ? Night::Night : Night::Day;
			if (n != night_) {
				char s[96];
				std::snprintf(s, sizeof(s), "night: %s -> %s (DA01.0C0D byte 0 = %d)",
				              night_name(night_), night_name(n), dimming_);
				out.push_back(s);
				night_ = n;
				night_changes_++;
			}
		}
		if (!k.seen) {
			k.seen = true;
			k.value = k.logged = m.payload;
			k.last_log_ms = ms;
			out.push_back(key + " first [" + ipc4::hex(m.payload, 24) + "]");
			return;
		}
		if (k.value == m.payload)
			return;
		k.value = m.payload;
		k.window++;
		// the light functions are rare and each value counts: never held
		bool every = m.fblock == ipc4::kCarFblock &&
		             (m.fct == kDimmingFct || m.fct == ipc4::kExteriorLightFct);
		if (every || (ms - k.last_log_ms >= gap_ms_ && !k.pending)) {
			out.push_back(key + " [" + ipc4::hex(k.logged, 24) + "] -> [" +
			              ipc4::hex(k.value, 24) + "]");
			k.logged = k.value;
			k.last_log_ms = ms;
			return;
		}
		k.pending = true;
		k.held++;
	}

	// Call regularly: logs the held changes whose gap has passed.
	void tick(unsigned ms, std::vector<std::string> &out)
	{
		for (auto &[key, k] : keys_)
			if (k.pending && ms - k.last_log_ms >= gap_ms_)
				release(key, k, ms, out);
	}

	// Everything held, now (at the end).
	void flush(unsigned ms, std::vector<std::string> &out)
	{
		for (auto &[key, k] : keys_)
			if (k.pending)
				release(key, k, ms, out);
	}

	// "night unknown (byte 0 -); changes: DA01.0C10/C driving state x12, ..."
	// for the window since the last call; resets the window's counts.
	std::string window()
	{
		char s[96];
		if (dimming_ < 0)
			std::snprintf(s, sizeof(s), "night %s (byte 0 -)", night_name(night_));
		else
			std::snprintf(s, sizeof(s), "night %s (byte 0 = %d)", night_name(night_), dimming_);
		std::string line = s;
		std::string ch;
		for (auto &[key, k] : keys_) {
			if (k.window == 0)
				continue;
			ch += (ch.empty() ? "" : ", ") + key + " x" + std::to_string(k.window);
			k.window = 0;
		}
		return line + "; changes: " + (ch.empty() ? "none" : ch);
	}

	Night night() const { return night_; }
	int dimming() const { return dimming_; }          // -1: not seen yet
	u32 night_changes() const { return night_changes_; }
	usize keys() const { return keys_.size(); }

private:
	struct Key {
		bool seen = false, pending = false;
		std::vector<u8> value, logged;   // now; as of the last line
		unsigned last_log_ms = 0;
		u32 held = 0, window = 0;
	};

	void release(const std::string &key, Key &k, unsigned ms, std::vector<std::string> &out)
	{
		char s[64];
		std::snprintf(s, sizeof(s), " (%u changes in %u.%u s)", k.held,
		              (ms - k.last_log_ms) / 1000, (ms - k.last_log_ms) % 1000 / 100);
		out.push_back(key + " [" + ipc4::hex(k.logged, 24) + "] -> [" + ipc4::hex(k.value, 24) +
		              "]" + s);
		k.logged = k.value;
		k.last_log_ms = ms;
		k.pending = false;
		k.held = 0;
	}

	unsigned gap_ms_;
	std::map<std::string, Key> keys_;
	Night night_ = Night::Unknown;
	int dimming_ = -1;
	u32 night_changes_ = 0;
};

} // namespace carsig

#endif

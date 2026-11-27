// carsig.h - the car's own signals on /dev/ipc/ch4 (docs/vehicle-data.md
// §5.1-5.2), as log lines: the first value and every change of every
// function, at most one line per function per gap (the changes in between
// are counted and the latest value logged when the gap has passed), and the
// night state. Gaps: none for the light functions 0x0C0C / 0x0C0D, car_gap
// for the car-service blocks DA01 / D901 / FB01, min_gap for the rest
// (cluster, unknown blocks, BAP). BAP streams are split per control unit
// and function ("BAP 0B.1A"); the car's clock and a counter that change
// every second are left out (kNoisyBap).
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

// What a function is, from MMI3GApplication's car-service dispatch
// (0x083c5414 / 0x083c5588) and its handlers' log strings; "" unknown. The
// payload's byte layout is not decoded: the handlers read deserialised
// fields.
inline const char *name(const ipc4::Most &m)
{
	if (m.fblock == ipc4::kCarFblock) {
		switch (m.fct) {
		case 0x0C0A: return "clamp/energy (clamp 15, battery charge, switch-off)";
		case 0x0C0C: return "exterior light";
		case 0x0C0D: return "dimming (night)";
		case 0x0C0E: return "sound info (engine rpm, blower, parking aid)";
		case 0x0C10: return "driving state (speed, stop, direction)";
		case 0x0C11: return "driving info (fuel warning, tank)";
		case 0x0C12: return "oil level cyclic";
		case 0x0C13: return "trailer";
		case 0x0C1D: return "car service 0C1D";
		case 0x0C36: return "crash";
		case 0x0C3D: return "offroad (steering angle, locks, low gear)";
		default: return "";
		}
	}
	if (m.fblock == 0xD9) {
		switch (m.fct) {
		case 0x0531: return "oil level";
		case 0x0540: return "car service D9 0540";
		default: return "";
		}
	}
	if (m.fblock == 0xDD)
		return "cluster";
	return "";
}

// The car-service blocks: logged with the short gap.
inline bool is_car_service(u8 fblock)
{
	return fblock == ipc4::kCarFblock || fblock == 0xD9 || fblock == 0xFB;
}

// BAP functions that change every second whatever the car does: the
// clock (LSG 0x11 fid 0x11, hh mm ss) and a counter (LSG 0x0F fid 0x2A).
inline bool is_noisy_bap(const ipc4::BapFn &f)
{
	return (f.lsg == 0x11 && f.fid == 0x11) || (f.lsg == 0x0F && f.fid == 0x2A);
}

enum class Night { Unknown, Day, Night };

inline const char *night_name(Night n)
{
	return n == Night::Night ? "NIGHT" : n == Night::Day ? "day" : "unknown";
}

class Logger {
public:
	explicit Logger(unsigned min_gap_ms = 5000, unsigned car_gap_ms = 1000)
		: gap_ms_(min_gap_ms), car_gap_ms_(car_gap_ms)
	{
	}

	// One telegram's function; a BAP stream is fed per function. Lines for
	// the log go to out.
	void feed(unsigned ms, const ipc4::Most &m, std::vector<std::string> &out)
	{
		if (ipc4::is_bap(m)) {
			std::vector<ipc4::BapFn> fs;
			ipc4::parse_bap(m.payload, fs);
			for (const ipc4::BapFn &f : fs)
				if (!is_noisy_bap(f))
					change(ms, ipc4::bap_key(f), f.payload, gap_ms_, out);
			return;
		}
		std::string key = ipc4::most_key(m);
		const char *nm = name(m);
		if (*nm)
			key += std::string(" ") + nm;
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
		// the light functions are rare and each value counts: never held
		bool light = m.fblock == ipc4::kCarFblock &&
		             (m.fct == kDimmingFct || m.fct == ipc4::kExteriorLightFct);
		unsigned gap = light ? 0 : is_car_service(m.fblock) ? car_gap_ms_ : gap_ms_;
		change(ms, key, m.payload, gap, out);
	}

	// Call regularly: logs the held changes whose gap has passed.
	void tick(unsigned ms, std::vector<std::string> &out)
	{
		for (auto &[key, k] : keys_)
			if (k.pending && ms - k.last_log_ms >= k.gap_ms)
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
		unsigned last_log_ms = 0, gap_ms = 0;
		u32 held = 0, window = 0;
	};

	// One function's value: the first one, or a change (at once, or held
	// until gap_ms has passed since the last line).
	void change(unsigned ms, const std::string &key, const std::vector<u8> &v, unsigned gap_ms,
	            std::vector<std::string> &out)
	{
		Key &k = keys_[key];
		k.gap_ms = gap_ms;
		if (!k.seen) {
			k.seen = true;
			k.value = k.logged = v;
			k.last_log_ms = ms;
			out.push_back(key + " first [" + ipc4::hex(v, 24) + "]");
			return;
		}
		if (k.value == v)
			return;
		k.value = v;
		k.window++;
		if (gap_ms == 0 || (ms - k.last_log_ms >= gap_ms && !k.pending)) {
			out.push_back(key + " [" + ipc4::hex(k.logged, 24) + "] -> [" +
			              ipc4::hex(k.value, 24) + "]");
			k.logged = k.value;
			k.last_log_ms = ms;
			return;
		}
		k.pending = true;
		k.held++;
	}

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

	unsigned gap_ms_, car_gap_ms_;
	std::map<std::string, Key> keys_;
	Night night_ = Night::Unknown;
	int dimming_ = -1;
	u32 night_changes_ = 0;
};

} // namespace carsig

#endif

// vcap.h - whether the HMI owns VCAP 0 (the rear camera), from
// dev-videoctrl's slog lines. Pure, tests/test_camera.cpp. Why the slog:
// docs/rear-camera.md §9.1.
#ifndef CAMERA_VCAP_H
#define CAMERA_VCAP_H

#include "types.h"

#include <cstddef>
#include <cstring>

namespace camera {

// The state, from dev-videoctrl's lines:
//   "VP0: source changed to N"  only the HMI writes VP0, and always when it
//                               starts a path through the ADV7180
//   "VP1: vcap disabled"        the HMI's stop path. While the HMI owns VCAP 0
//                               we don't write VP1, so this one is the HMI's
class VcapTracker {
public:
	void line(const char *text, usize n, unsigned now_ms)
	{
		if (has(text, n, "VP0: source changed to")) {
			active_ = true;
		} else if (active_ && has(text, n, "VP1: vcap disabled")) {
			active_ = false;
			ended_ = true;
			ended_ms_ = now_ms;
		}
	}

	// Events as read() from /dev/slog gives them, whole: 3 ints of header
	// (flags and int count, code, time), then the data.
	void events(const int *ev, usize n_ints, unsigned now_ms)
	{
		usize i = 0;
		while (i + 3 <= n_ints) {
			usize count = ((unsigned)ev[i] >> 16) & 0xff;
			bool text = (ev[i] & 0x8) != 0;
			if (i + 3 + count > n_ints)
				break;
			if (text) {
				const char *t = reinterpret_cast<const char *>(ev + i + 3);
				usize max = count * sizeof(int);
				const void *end = std::memchr(t, 0, max);
				line(t, end ? (usize)(static_cast<const char *>(end) - t) : max, now_ms);
			}
			i += 3 + count;
		}
	}

	bool active() const { return active_; }
	bool ended() const { return ended_; }
	unsigned ended_ms() const { return ended_ms_; }

private:
	static bool has(const char *text, usize n, const char *what)
	{
		usize w = std::strlen(what);
		for (usize i = 0; i + w <= n; i++)
			if (std::memcmp(text + i, what, w) == 0)
				return true;
		return false;
	}

	bool active_ = false;
	bool ended_ = false;
	unsigned ended_ms_ = 0;
};

} // namespace camera

#endif

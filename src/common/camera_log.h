// camera_log.h - whether the HMI owns VCAP 0 (the rear camera), from
// dev-videoctrl's slog lines, and what an app with a picture does about it.
// Pure, tests/test_camera.cpp. Why the slog: docs/rear-camera.md §9.
#ifndef COMMON_CAMERA_LOG_H
#define COMMON_CAMERA_LOG_H

#include <cstddef>
#include <cstring>

namespace camera {

// After the camera's capture goes off, the HMI still has to show its next
// context. A show of ours before that would be replaced.
constexpr unsigned kSettleMs = 1000;
// A camera that starts this soon after we retook the screen may have written
// before us: put its path back.
constexpr unsigned kVerifyMs = 2000;
// The HMI may show once more after we retook the screen; nothing tells us, so
// show again once.
constexpr unsigned kShowAgainMs = 2000;

// The state, from dev-videoctrl's lines:
//   "VP0: source changed to N"  only the HMI writes VP0, and always when it
//                               starts a path through the ADV7180
//   "VP1: vcap disabled"        the HMI's stop path. While the HMI owns VCAP 0
//                               we don't write VP1, so this one is the HMI's
class Tracker {
public:
	void line(const char *text, size_t n, unsigned now_ms)
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
	void events(const int *ev, size_t n_ints, unsigned now_ms)
	{
		size_t i = 0;
		while (i + 3 <= n_ints) {
			size_t count = ((unsigned)ev[i] >> 16) & 0xff;
			bool text = (ev[i] & 0x8) != 0;
			if (i + 3 + count > n_ints)
				break;
			if (text) {
				const char *t = reinterpret_cast<const char *>(ev + i + 3);
				size_t max = count * sizeof(int);
				const void *end = std::memchr(t, 0, max);
				line(t, end ? (size_t)(static_cast<const char *>(end) - t) : max, now_ms);
			}
			i += 3 + count;
		}
	}

	bool active() const { return active_; }
	bool ended() const { return ended_; }
	unsigned ended_ms() const { return ended_ms_; }

private:
	static bool has(const char *text, size_t n, const char *what)
	{
		size_t w = std::strlen(what);
		for (size_t i = 0; i + w <= n; i++)
			if (std::memcmp(text + i, what, w) == 0)
				return true;
		return false;
	}

	bool active_ = false;
	bool ended_ = false;
	unsigned ended_ms_ = 0;
};

// One owner of a picture (the video displayable, or the CPU-drawn screen).
// Call tick() regularly and do what it says.
class Takeover {
public:
	enum class Action {
		None,
		Retake,     // the camera is gone: show the picture again
		ShowAgain,  // once more, for a late show of the HMI
		GiveBack,   // the camera started as we retook: it comes first
	};

	Action tick(bool have_picture, bool active, bool clear, unsigned now_ms)
	{
		if (active) {
			bool just_retook = retook_ && now_ms - retook_ms_ < kVerifyMs;
			retook_ = false;
			again_ = false;
			if (have_picture || just_retook)
				taken_ = true;
			return just_retook ? Action::GiveBack : Action::None;
		}
		if (!have_picture) {
			taken_ = retook_ = again_ = false;
			return Action::None;
		}
		if (taken_ && clear) {
			taken_ = false;
			retook_ = again_ = true;
			retook_ms_ = now_ms;
			return Action::Retake;
		}
		if (again_ && now_ms - retook_ms_ >= kShowAgainMs) {
			again_ = false;
			return Action::ShowAgain;
		}
		return Action::None;
	}

	bool taken() const { return taken_; }

private:
	bool taken_ = false;
	bool retook_ = false;
	bool again_ = false;
	unsigned retook_ms_ = 0;
};

} // namespace camera

#endif

// takeover.h - what an app with a picture does when the rear camera comes
// and goes (docs/rear-camera.md §9.1). Pure, tests/test_camera.cpp.
#ifndef CAMERA_TAKEOVER_H
#define CAMERA_TAKEOVER_H

namespace camera {

// A camera that starts this soon after we retook the screen may have written
// before us: put its path back.
constexpr unsigned kVerifyMs = 2000;
// The HMI may show once more after we retook the screen; nothing tells us, so
// show again once.
constexpr unsigned kShowAgainMs = 2000;

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

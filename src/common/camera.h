// camera.h - is the HMI using VCAP 0 for the rear camera right now? A thread
// follows dev-videoctrl's lines in /dev/slog (camera_log.h), reading only:
// nothing here touches the video path. docs/rear-camera.md §9.
//
// Not started (or no /dev/slog): active() is false and clear() true, so the
// callers behave as if there were no camera.
#ifndef COMMON_CAMERA_H
#define COMMON_CAMERA_H

namespace camera {

// Reads the slog so far before it returns, so active() is right at once.
void start();
void stop();

// The HMI owns VCAP 0: don't write VP1, don't show anything.
bool active();
// Not active, and the HMI has had time to show its next context.
bool clear();

} // namespace camera

#endif

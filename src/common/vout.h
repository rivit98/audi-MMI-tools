// vout.h - put the DSP's decoded picture on screen: a layer-manager video
// displayable bound to VCAP 0, and VCAP 0 switched to input 1 (the DM642's
// video port) through dev-videoctrl. The path hello6 found and
// sees on every run (docs/phone-mirroring.md 2.2, 7.4).
#ifndef COMMON_VOUT_H
#define COMMON_VOUT_H

namespace vout {

// VCAP 0's field mode (dev-videoctrl: BOB, WEAVE, ODD ONLY). Keep leaves it
// as it is.
enum class Mode { Keep, Bob, Weave, OddOnly };
const char *mode_name(Mode m);

// Set the mode, if it isn't that already. Keep after another mode goes back
// to WEAVE, the HMI's value for the rear camera: the HMI trusts its cache and
// may not send it again (docs/rear-camera.md).
int set_mode(Mode m);

// Register + show the video displayable (cid 41, flags 0x101), set the mode
// and enable capture. Returns 0 if every step said ok (dev-videoctrl answers
// EOK even to payloads it ignores; its slog lines confirm).
int show(Mode mode = Mode::Keep);
// Capture off, VCAP 0 back to input 0 (the ADV7180 camera input, as at
// boot), mode as in set_mode(Keep), displayable gone. restore_vcap false:
// source and capture are left as they are - for when something else (the
// HMI's rear camera, docs/rear-camera.md §9) may own VCAP 0 by now: writing
// ENABLE 0 then would hide the camera. The mode still goes back.
void hide(bool restore_vcap = true);
// Close /dev/videoctrl (end of the run).
void close_all();

} // namespace vout

#endif

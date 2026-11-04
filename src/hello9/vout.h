// vout.h - put the DSP's decoded picture on screen: a layer-manager video
// displayable bound to VCAP 0, and VCAP 0 switched to input 1 (the DM642's
// video port) through dev-videoctrl. The path hello6 found on 19.10 v2 and
// saw on every run since (docs/phone-mirroring.md 2.2, 7.4).
#ifndef HELLO9_VOUT_H
#define HELLO9_VOUT_H

namespace vout {

// Register + show the video displayable (cid 41, flags 0x101) and enable
// capture. Returns 0 if every step said ok (dev-videoctrl answers EOK even
// to payloads it ignores; its slog lines confirm).
int show();
// Capture off, VCAP 0 back to input 0 (the ADV7180 camera input, as at
// boot), displayable gone.
void hide();
// Close /dev/videoctrl (end of the run).
void close_all();

} // namespace vout

#endif

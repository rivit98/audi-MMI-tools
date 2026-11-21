# hello10

Crop test: can the DM642 show an 800-wide H.264 stream whose SPS is
rewritten to say 720 (`h264::crop_sps_annexb`)? Android Auto's smallest
video is 800×480. C++. Retired.

- **Run:** `hello10 <sd_path> [test,test,...]` (default `c5,crop,cropnav,slow`)
- **Output:** `var/hello10.log` (SPS before/after, `RESULT` lines), `var/hello10/` (queries, slog)

## Verifies

- `c5`: control.
- `crop`: 800×480 coded, cropped to 720×480.
- `cropnav`: cropped to 720×416 (cut Android Auto's bottom bar).
- `slow`: the same at 15 fps with skip frames.

## Results

- **Works:** `c5` plays; io-media accepts the cropped stream (720×480 / 720×416, TIME events, 0 errors).
- **Doesn't work:** `crop`, `cropnav`, `slow` stay black. No DSP error; the
  unit runs on. An 800-wide coded picture can't be shown, cropped or not.

Details: [docs/android-auto.md §4.1](../../docs/android-auto.md).

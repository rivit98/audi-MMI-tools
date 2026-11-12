# hello10

The crop test: can the unit's DM642 show an **800-wide** H.264 stream
whose SPS says 720? Android Auto's smallest video is 800×480, and io-media
refuses anything wider than 720 (`FUN_080d1760`,
[docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.8). With an
80 px width margin the phone keeps its UI in the left 720 columns, and the
MMI rewrites the SPS so the stream says 720×480
([docs/android-auto.md](../../docs/android-auto.md) §4.1). The coded
picture stays 800 wide (50 macroblocks). Whether the DSP decodes it, trips
its `width < 720` assertion or garbles it is what hello10 answers. Written
in C++ ([docs/cpp-apps.md](../../docs/cpp-apps.md)).

| Test | What | Question |
|---|---|---|
| `c5` | plain MP4 on the card (720×336), played as a file | Control: black here means the display path broke, not the crop |
| `crop` | c12 cropped to 720×480, 25 fps, 30 s | Does an 800-wide coded picture play? A **red "CUT" band** on the right = the DSP ignores the crop |
| `cropnav` | c12 cropped to 720×416, 20 s | Is the **blue band** at the bottom gone, and is the picture stretched to the full height? (cutting Android Auto's bottom bar) |
| `slow` | as `crop`, frames at 15 fps, gap fill to 25, 20 s | Skip frames on an 800-wide coded picture: what a paced phone sends |

The default is all four, about 2.5 minutes.

How each piece works:

- **The clip:** `c12_h264_800.h264` (`media/mkclips.sh`), raw Annex-B
  800×480 Baseline, **Level 3.1** as the phone sends it: the moving picture
  (label, counter) in the left 720 columns, a red band in the right 80, a
  blue band in the bottom 64 lines. 550 frames, an IDR every 25, looped.
- **The rewrite:** `h264::crop_sps_annexb` (`src/common/h264.h`) rewrites
  every SPS in the clip: frame cropping to the test's size (right and
  bottom), `level_idc` 3.0, everything else (the VUI included) bit for
  bit. 800×480 at 25 fps is 37 500 macroblocks/s, within Level 3's 40 500,
  so the level the stream claims is true. `make test` checks the rewrite:
  ffmpeg decodes a cropped clip, raw and as a CS-MP4, to exactly the
  original's top-left (`tests/check_live.py`).
- **The stream:** the rewritten clip goes through `HwStream`
  (`src/common/hw_stream.h`), the code audi_stream_receiver uses: CS-MP4
  header (it declares the cropped size), frames padded to S, the resource
  manager at `/dev/hello10/live.mp4`, gap fill, VCAP 0 input 1. A feeder
  thread pushes frame k at k / fps. hello9's README describes these parts.
- **The driver** turns the knob once when the moving picture appears, and
  films the screen: the bands show only on video.

**Risk:** the DSP also runs the unit's audio. If its width assertion
fires, sound or video may stay dead until the MMI restarts (MENU + main
knob + top-right key). hello10 says so on its first screen; the control
runs first.

## Running

- **Run:** `hello10 <sd_path> [test,test,...]`, e.g. `crop`. `make sd
  SD_APPS="hello10 ..."` puts the clips (c5, c12) on the card.
- **Output** (in `var/` on the card):
  - `hello10.log`: the SPS as sent and after the rewrite, a status line per
    second for the live tests (frames pushed, skips, reads, position, TIME
    events, errors, lag), then `RESULT` lines and the summary.
  - `hello10/`: `query_<test>.log` (io-media's view of the graph:
    `GetTrackInfo`, `GetVideoInfo`), `slog.txt` (the system log after the
    tests: io-media's and the DSP's messages, an assertion if one fired).

## Reading the result

The `RESULT` lines have hello9's fields (its README): `playing_ms`,
`time_events`, `pos_ms`, `errors`, `video` and `WxH` from the query,
`knob`, `lag`. For a crop test:

| Seen | Meaning |
|---|---|
| picture, no red band, `time_events` rising | the crop works: Android Auto video is wiring |
| picture with the red band | the DSP ignores the crop and shows 800 columns into 720 |
| black, `errors` > 0 or no TIME events | refused or the DSP stopped: `slog.txt` for an assertion |
| `cropnav`: blue band gone, picture full height | the vertical crop works; the VCAP stretches 416 lines to 480 |

## State

Built, host-tested (the SPS rewrite); not yet run on the car.

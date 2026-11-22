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
| `probe` | read only: io-media-nvidia's three capture-width words through `/proc/<pid>/as`; its `pidin arg` / `env`; its segment list (`DCMD_PROC_MAPINFO`, flags named by `mapinfo.cpp`); the width word's page mapped `MAP_SHARED` from `/mnt/efs-system/sbin/io-media-nvidia` in our own process and its `posix_mem_offset64` | Are the addresses right? How is io-media's code mapped, and is that page RAM or flash (phone-mirroring §7.14)? |
| `cap800` | c3 (720×480) with io-media's capture width written to 800 in memory, 30 s | Does the media port capture an 800-sample line? (phone-mirroring §7.14) |
| `seq` | c3 at 720, `cap800`, c3 at 720 again | The same, with a control before and after |
| `sw` | c12 (800×480) decoded in **software** on the SH4: 20 s decode only, then on screen through `gf_draw_image` as BGRA (15 s) and as YUV (10 s) | How many frames a second the SH4 decodes, and how many reach the screen. Section "Software decoder" |

The default is `probe`, `seq` (the capture width test) and then `sw`, about 3 minutes.

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

## Software decoder (`sw`)

The DM642 path can't show 800 wide. Decoding in software on the SH4 has no
width limit, and the two costs are measured separately:

- **Decoding:** `common/swdec.c` over h264bsd (Android's Baseline decoder,
  Apache-2.0, fetched by the Makefile at a pinned commit). The wrapper adds
  an access unit delimiter to each frame, so every picture comes out of its
  own access unit with no frame of delay. `tests/test_swdec.cpp` checks it
  against ffmpeg, byte for byte, single and multi-slice, with cropping.
- **Getting the picture on screen:** CPU writes into the VFB are slow,
  ~10 MB/s, so a full 800×480 BGRA copy is ~160 ms
  ([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §2.4). `sw`
  hands the picture to libgf's `gf_draw_image` instead, so the 2D engine
  copies it from RAM into the VFB. A wired CarPlay project on a K0742 unit
  uses this to draw a 320×192 displayable [unverified on P1001]. The
  `yuv` step tries `GF_FORMAT_PLANAR_YUV_420` straight from the decoder,
  then UYVY packed by the CPU (half of BGRA's bytes). Either way the
  graphics chip converts the colours, not the CPU.

That project's quoted 31 fps for 800×480 is decode only, on short clips.
Its picture reaches the screen at 320×192.

The log has a line every 5 s and one `RESULT sw_decode` / `sw_bgra` /
`sw_yuv` line each: fps, and min/median/p90/max ms for decode, convert,
draw (`gf_draw_image` + `gf_draw_finish`) and flip (`updateVfb` +
`getVfb`). `sw_yuv`'s `format=` says which YUV format libgf took.

| Seen in `sw` | Meaning |
|---|---|
| the c12 picture (counter, red band right, blue band bottom), normal colours | `gf_draw_image` works from RAM |
| `bgra` picture fine, `yuv` wrong colours / striped | the 2D engine takes the format but reads it differently (the format's layout is a guess) |
| black, `gf_draw_image` ≠ 0 in the log | libgf refuses that format or size |

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

## Results

**Works**
- `seq` videos 1 and 3 (c3, 720×480 through the DSP): picture on screen,
  knob turns seen, io-media reports 720×480, no errors; lag median ~330 ms
  and ~540 ms.
- `sw` decoding: 800×480 Baseline at **37.6 fps** (752 frames in 20 s),
  decode ms min/median/p90/max 22/25/28/63, IDR ~54 ms, P ~25 ms, no errors.
- `gf_draw_image` returns 0 for `BGRA8888` and for `PLANAR_YUV_420`; the
  loop runs at 12.1 fps (BGRA: decode 26 + CPU convert 33 + flip 23 ms)
  and 21.6 fps (I420: decode 26 + flip 20 ms, no convert).

**Doesn't work**
- `cap800` (video 2): the in-memory patch is refused. The three words read
  720, the first write fails with `ESRVRFAULT` ("Server fault on msg
  pass"), nothing is changed and video 2 is skipped (phone-mirroring §7.14).
- `sw` on screen: **no decoded picture is visible**. During both drawing
  steps the screen alternates between the previous text screen (the step's
  instructions, left in the VFB) and the HMI's radio screen showing
  through, about every 2–4 s, though every draw and flip returns 0. So the
  2D engine's writes don't reach the surface the layer manager shows, or
  arrive transparent (cid 40 is a "Transparent" displayable,
  platform-notes §3.1) [inferred, cause not known]. The fps figures above
  are the loop's, not frames seen.

## State

Run on the car (`seq` + `sw`). The extended `probe` (mappings, physical
address, io-media's arguments and environment) is built and host-tested
(`tests/test_hello10_mapinfo.cpp`), not yet run on the car. Open: why
`gf_draw_image` output stays invisible; the capture-width probe needs a
patched io-media copy (or a RAM page patch, if `probe` finds one) instead of
the `/proc` write.

`probe` output: `maps:` lines (each width word's segment: flags, dev, ino,
offset, what a `/proc` write there does; the same for hello10's own code,
run from the card), a `phys:` line (`posix_mem_offset64` of our mapping of
the file: rc, offset = physical address when it is plain memory, fd), and
`var/hello10/iom_maps.txt`, `iom_arg.txt`, `iom_env.txt`, `asinfo.txt`
(`pidin syspage=asinfo`: which physical ranges are RAM and which flash).

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
| `iomcopy` | a second io-media-nvidia from RAM next to the HMI's own: an unchanged copy plays c3 (10 s), then a copy with the capture width at 800 plays it (20 s), then the original plays it (8 s). Section "io-media copy" | Can a copy run beside the original, and does the media port capture an 800-sample line (phone-mirroring §7.14)? |

The default is `probe` and then `iomcopy`, about 3 minutes. `seq`
runs by name. `iomcopy` is last: it may leave the radio or the screen
broken until the MMI restarts.

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

## io-media copy (`iomcopy`)

io-media's capture width can't be changed in the running process (the
`/proc` write is refused, phone-mirroring §7.14), so `iomcopy` runs a
second io-media-nvidia from RAM and leaves the HMI's alone.

- **The copy:** `/mnt/efs-system/sbin/io-media-nvidia` copied to the run
  directory as `iom-copy` (or `iom-copy800` with the three width words
  `d0 02` → `20 03` at file offsets 0x14a190, 0x14a26e, 0x14ad20; each
  must read 720 first). It is started with the original's `/etc/iom.cfg`
  plus `mountpoint = "/dev/hello10-iom"` in `global-options`
  (`iomcopy_cfg.h`, host-tested, checked on the unit's files), `-B` and
  srv-starter's `NV_*` environment. Nothing is written to flash.
- **Why it can share the unit [inferred from the P1001 binaries]:** the
  config's `mountpoint` replaces `/dev/io-media` (option parsing at
  `0x080892b0`); `-B` keeps it in the foreground (without it,
  `procmgr_daemon` gives it a new pid); io-media is a client of
  `devg-NVTS` (`/dev/nvdsprm0`) and `devg-NVMiniRM` (`/dev/nvminirm0`),
  and the DSP image is loaded once at boot by NvElfLoader, so the copy
  doesn't reload the DSP.
- **Not known:** whether `devg-NVTS` serves a second client, and what the
  copy's start-up does to the DSP tasks the original runs (the audio
  mixer, the health monitor). Hence the steps below and the radio left on.
- **Steps:** start the copy, wait for its mountpoint (15 s at most), 5 s
  idle, then play c3 through it (our player pointed at the copy with
  `iom_set_root`), stop it (TERM, KILL). `copy800` runs only if `copy720`
  played. `orig720` plays c3 through the HMI's io-media again.
- **Logged:** the copy's start (pid, time to the mountpoint), what appeared
  in `/dev`, `/dev/name/*`, `/dev/shmem`, `/dev/tsshmem` while it started,
  whether the copy and the original are alive after the idle and after the
  play, how the copy ended, and a `RESULT copy720` / `copy800` / `orig720`
  line each (the fields above; `query` is empty for the copy:
  testapp-cmdline knows only `/dev/io-media`). Files in `var/hello10/`:
  `iom-copy*_out.txt` (the copy's output), `iom-copy*_ls_before.txt`,
  `_ls_after.txt`.

| Seen | Meaning |
|---|---|
| `copy720` plays (TIME events, picture), radio on, `orig720` plays | a copy works beside the original |
| `copy800`: right 80 columns black or frozen | the media port stops at the line's end: an 800-wide signal from a patched DSP image would be captured |
| `copy800`: garbled or slanted picture | the port counts pixels: 800 needs a matching signal, or is out |
| no mountpoint, or the copy exits | it can't run beside the original (its `_out.txt` and the slog say why) |
| radio silent or HMI media broken afterwards | the copy disturbed the DSP; restart the MMI |

## Running

- **Run:** `hello10 <sd_path> [test,test,...]`, e.g. `crop`. `make sd`
  puts the clips (c5, c12) on the card.
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

**Doesn't work**
- `cap800` (video 2): the in-memory patch is refused. The three words read
  720, the first write fails with `ESRVRFAULT` ("Server fault on msg
  pass"), nothing is changed and video 2 is skipped (phone-mirroring §7.14).
- **`probe`**: the width page is RAM shared by every io-media process, not
  flash; its physical address is still unknown (phone-mirroring §7.14).

## State

Run on the car: `seq` and `probe` (results above). Not yet run:
`iomcopy`. Open: whether the media port captures an 800-sample line
(`iomcopy`'s `copy800`). Software decoding on the SH4 was tried and
dropped: the picture reaches the screen at ~5 fps at best
([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §4).

`probe` output: `maps:` lines (each width word's segment: flags, dev, ino,
offset, what a `/proc` write there does; the same for hello10's own code,
run from the card), a `phys:` line (`posix_mem_offset64` of our mapping of
the file: rc, offset = physical address when it is plain memory, fd), and
`var/hello10/iom_maps.txt`, `iom_arg.txt`, `iom_env.txt`, `asinfo.txt`
(`pidin syspage=asinfo`: which physical ranges are RAM and which flash).

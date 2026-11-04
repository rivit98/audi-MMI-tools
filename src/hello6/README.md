# hello6

Checks the assumptions behind phone screen mirroring
([docs/phone-mirroring.md](../../docs/phone-mirroring.md)) in four phases:

- **`s`:** system info
- **`c`:** CPU benchmarks: memcpy, pixel conversion, LZ4
- **`v`:** video. Ten test clips are decoded by the unit's DM642 DSP
  through io-media and shown on a video layer, with capture enabled
  through `/dev/videoctrl`. The driver turns the knob when the picture is
  visible.
- **`g`:** GPU upload and draw benchmarks

Text screens say what is happening and what to do. A summary screen comes
at the end.

- **Run:** `hello6 <sd_path> [phases]` (default `v`, about 6 min). The
  clips (its 8 of them) go on the card when hello6 is in `SD_APPS`.
- **Output:** `hello6.log` (`RESULT` lines, summary) and `hello6/`
  (sysinfo, per-clip player logs, io-media queries, CPU load, system log).

Since 2026-10-30 the clips are 22 s (were 30 s) and each capture attempt
7 s (was 10 s). The driver saw the picture and turned the knob 2.4–4 s
into the attempt every time, and that saves about a minute per run.

**Out of the default run since 2026-11-03, done.** Its ten clips gave
the same answer four runs in a row (21.10, 25.10, 30.10, 03.11).

**The result: the unit's DM642 DSP decodes video and it reaches the
screen** on a layer-manager video displayable (flags `0x101`) with
VCAP 0 switched to input 1 through `/dev/videoctrl`:
- **Played and seen:** H.264 Baseline 720×480 and 720×336 in plain MP4,
  MPEG-2 720×576, MPEG-4 Part 2 640×480.
- **Not seen:** VCAP 1 never showed anything.
- **Never started:** clips 4 and 7–10, refused by io-media at open:
  - 800 wide: io-media's own size check refuses anything over 720×576.
  - Fragmented MP4: no samples in the header.
  - H.264 in MPEG-PS.
  - Growing copies of those.

This is the base of mirroring option 2a, and hello9's c5 control rechecks
it on every hello9 run.

**Still open, but not for hello6 to answer** (another run gives the same
result):
- Why H.264 in MPEG-PS fails.
- Whether io-media's own MP4 demuxer (`CDemuxMp4`, §7.9) really has no
  fragment support. The "no `trun`" reason came from `mp4_parser.so`.
- What VCAP 1 is for.
- H.264 at 720×576, the size limit, which was never played.

The streaming tests that came out of hello6 are
[hello9](../hello9/README.md)'s.

## On the car

**2026-11-03** (all ten clips, both ways, 368 s, after hello5;
`runs/3.11.2026 - run2/`, phone video for clips 1–3 and most of the rest)
gave the same result for the fourth time: clips 1, 2, 3, 5 and 6 played
their 22 s (35 TIME events) and were seen on VCAP 0 input 1, one knob
turn each. VCAP 1 had none. Clips 4 and 7–10 never started (`testapp-file`
killed with signal 9 at the end of each attempt). The same day hello9's
control clip (c5) played through hello9's own io-media client, so the
display path doesn't need `testapp-file`.

**2026-10-30** (all ten clips, both ways, 430 s, after hello8 and hello5)
was the same as 21.10 and 25.10, clip for clip:
- Clips 1, 2, 3, 5 and 6 played their 30 s (47 TIME events), with one
  knob turn each on VCAP 0 input 1, 2.4–4.0 s after capture was enabled.
- VCAP 1 had no knob turns.
- Clips 4 and 7–10 never started.

After clip 6 the driver stopped watching, and the car was most likely
switched off (ch8 power-state sequence, `docs/platform-notes.md` §8). That is the
"interrupted after the 5th video" (the 5th one seen). The unit kept
running, and hello6 finished clips 7–10 and its summary on its own.
Those five fail without a picture anyway, so nothing was lost.

**2026-10-25** (all ten clips, both ways, 428 s) repeated 21.10 exactly,
on the log and on the phone video:

- **On screen through VCAP 0 input 1:** clips 1 (MPEG-2 720×576), 2
  (MPEG-4 640×480 AVI), 3 (H.264 720×480), 5 (H.264 720×336) and 6 (the
  same without audio). Each played its 30 s to the end (47 TIME events),
  and the driver turned the knob once for each.
- **VCAP 1 stayed black for all ten** (no knob turn, nothing on the video).
- **Never started:** clip 4 (H.264 800×480), 7 (fragmented MP4), 8 (H.264
  in MPEG-PS) and the growing files 9 and 10 (the same containers, written
  while playing). Each time io-media's `GetTrackInfo` failed with
  `iom_errno=0x80002`, no TIME event came, and hello6 killed the player
  after its cap.
- During clip 2's VCAP 1 attempt the car was put in reverse: the MMI's
  own rear-view camera took the screen (with its parking overlay) and gave
  it back afterwards. hello6 didn't notice; nothing else was affected.
- The car was driven during the test; nothing depended on that.

**Works**
- **Video decode and display:** these clips play on screen:
  - MPEG-2 720×576, seen live at 25 fps, 4:3, centred, with correct
    colours
  - MPEG-4 Part 2 640×480 (AVI)
  - H.264 Baseline 720×480 and 720×336 (MP4, with or without an audio
    track)

  For the last three, the driver's knob clicks confirm the picture.
- **What makes the picture appear:**
  - the output `mixer:DSPOUT_TDM2_20?delay=130`
  - a video displayable
  - `/dev/videoctrl` set to VP 1 source 1 (VCAP 0 input 1, flags
    `0x101`), then enabled and updated
- **dev-videoctrl confirms every enable and disable** in the system log.
- **The CPU stays 48–61 % idle while the DSP decodes.** The busiest threads
  are the SD card driver and `devg-NVMiniRM`.
- **CPU benchmarks:**

  | test | result |
  |---|---|
  | RGB565 → BGRA, one 800×480 frame | about 12 ms |
  | RAM store | 210–218 MB/s |
  | memcpy | about 107 MB/s |
  | LZ4 decompress, UI frame (10.5 % of original) | about 80 MB/s, about 9 ms per frame |
  | LZ4 decompress, photo (71 % of original) | about 25 MB/s |

- **System:** about 155 of 476 MB RAM free and about 124 MB of video memory
  available.

**Doesn't work**
- **VCAP 1 (VP 2)** stays black for every clip.
- **These clips fail at once with `iom_errno=0x80002`** and play nothing:
  - 800×480 H.264
  - fragmented MP4
  - H.264 in an MPEG program stream
  - files still being written
- **`testapp-file`'s default output won't attach** (`iom_errno=0x10001`).
  It needs a `-o mixer:...` output.
- **The GPU phase froze the whole unit,** at the first large texture upload.
- **Writing to the frame buffer is slow:** about 9 MB/s, or 161 ms per
  800×480 frame.

# hello6

Checks the assumptions behind phone screen mirroring (tests T1–T3 of
[docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7) in four phases:

| Phase | What | Output |
|---|---|---|
| `s` system info | `pidin`, mounts, `/dev/io-media`, `/dev/snd`, efs tool locations; the unit's `ShowVideoMemory`, `memusage`, `hbhogs` (not `ShowDisplayables`: `ShowDisplayables 0` blanked the screen) | `hello6/sysinfo_before.txt`, `sysinfo_after.txt` |
| `c` CPU | memcpy, RGB565→BGRA, LZ4 on UI/photo/random 800×480 frames | `RESULT` lines |
| `g` GPU | CPU stores/memcpy to and from the VFB; `glTexSubImage2D` RGB565/RGBA/BGRA and DXT1 upload MB/s; visible upload+draw fps | `RESULT` lines |
| `v` video | 10 clips of 22 s (`media/mkclips.sh`) played by the unit's `testapp-file` with `-o mixer:DSPOUT_*` (its default output can't attach), each shown on a video displayable (cid 41) two ways, 7 s each: VCAP 1 (flags `0x201`, VP 2), then VCAP 0 input 1 (`0x101`, VP 1 source 1, the DSP's video port). The driver turns the knob when the picture is visible. Clips 9–10 are written to the card while they play | `hello6/play_<k>.log`, `hogs_<k>.txt`, `RESULT video_<k>` lines |

The unit's tools come from its efs (`/mnt/efs-system/bin`), with this
firmware's copies on the card as a fallback
([docs/native-apps.md](../../docs/native-apps.md) §14). The clips:
`make clips`; the LZ4 test: `make test` (`test_lz4`, against liblz4).

Text screens say what is happening and what to do. A summary screen comes
at the end.

- **Run:** `hello6 <sd_path> [phases]` (default `v`, about 6 min). The
  clips (its 8 of them) go on the card when hello6 is in `SD_APPS`.
- **Output:** `hello6.log` (`RESULT` lines, summary) and `hello6/`
  (sysinfo, per-clip player logs, io-media queries, CPU load, system log).

Each capture attempt is 7 s: the driver sees the picture and turns the
knob 2.4–4 s into the attempt.

**Not in the default run: done.** Its ten clips give the same answer on
every run.

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
it on every hello9 run. c5 plays through hello9's own io-media client, so
the display path doesn't need `testapp-file`.

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

Per clip (log and phone video):

| clip | VCAP 0 input 1 | VCAP 1 |
|---|---|---|
| 1 MPEG-2 720×576 | seen, played to the end | black |
| 2 MPEG-4 640×480 AVI | seen, played to the end | black |
| 3 H.264 720×480 | seen, played to the end | black |
| 4 H.264 800×480 | never started | never started |
| 5 H.264 720×336 | seen, played to the end | black |
| 6 the same without audio | seen, played to the end | black |
| 7 fragmented MP4 | never started | never started |
| 8 H.264 in MPEG-PS | never started | never started |
| 9, 10 growing copies (written while playing) | never started | never started |

- A clip that plays runs its full length (35 TIME events for 22 s), with
  one knob turn on VCAP 0 input 1.
- A clip that never starts: io-media's `GetTrackInfo` fails with
  `iom_errno=0x80002`, no TIME event comes, and hello6 kills
  `testapp-file` (signal 9) at the end of the attempt.
- Putting the car in reverse during an attempt: the MMI's own rear-view
  camera takes the screen (with its parking overlay) and gives it back
  afterwards. hello6 doesn't notice; nothing else is affected.

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

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

- **Run:** `hello6 <sd_path> [phases]` (default `v`, about 8 min). The
  clips go on the card with `make sd SD_APPS="hello6 ..."`.
- **Output:** `hello6.log` (`RESULT` lines, summary) and `hello6/`
  (sysinfo, per-clip player logs, io-media queries, CPU load, system log).

## On the car

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

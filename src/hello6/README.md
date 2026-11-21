# hello6

Phone-mirroring groundwork (tests T1–T3 of [docs/phone-mirroring.md §7](../../docs/phone-mirroring.md)):
system info, CPU and GPU benchmarks, and 10 clips played through the DM642
DSP onto a video displayable (VCAP 0 input 1 and VCAP 1). Retired: every run
gives the same answer.

- **Run:** `hello6 <sd_path> [phases]` (`s` sysinfo, `c` CPU, `g` GPU, `v` video; default `v`)
- **Output:** `var/hello6.log` (`RESULT` lines), `var/hello6/` (sysinfo, player logs, slog)

## Verifies

- Can the DSP decode video and can it reach the screen?
- Which codecs, sizes and containers io-media accepts.
- CPU cost of conversions/LZ4, VFB write speed, texture upload speed.

## Results

**Works**
- DSP decode on screen via VCAP 0 input 1 (flags `0x101`, `/dev/videoctrl` VP 1 source 1):
  MPEG-2 720×576, MPEG-4 Part 2 640×480, H.264 Baseline 720×480 / 720×336 (plain MP4).
- CPU stays 48–61 % idle while the DSP decodes.
- RGB565→BGRA ~12 ms per frame; LZ4 UI frame ~9 ms.

**Doesn't work**
- VCAP 1 stays black.
- Refused at open (`iom_errno=0x80002`): 800-wide H.264, fragmented MP4,
  H.264 in MPEG-PS, files still being written.
- `testapp-file` needs `-o mixer:…`; its default output won't attach.
- CPU writes to the VFB: ~9 MB/s (161 ms per frame).
- The GPU phase froze the unit at the first large texture upload.

Live streaming follow-up: [hello9](../hello9/README.md).

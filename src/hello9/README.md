# hello9

Live H.264 streaming through the DM642 with io-media untouched. Runs the
receiver's code (`src/common/hw_stream.h`: CS-MP4 header, own resource
manager, gap fill) fed from a clip on the card, the way a phone would feed
it. C++. Retired: all answered.

- **Run:** `hello9 <sd_path> [test,test,...]` (default: every test, `c5,live,gap,slow,burst,s56,s112,f30,c9,stall`)
- **Output:** `var/hello9.log` (`RESULT` lines, lag), `var/hello9/` (io-media queries, slog)

## Verifies

- `c5`: control, plain MP4 file.
- `live`: the MMI-built stream, 25 fps, 30 s; lag.
- `gap`: 2 s and 5 s sender pauses filled with skip frames.
- `slow` / `burst`: 15 fps sender, clumped arrival.
- `s56` / `s112` / `f30`: is io-media's queue bytes or frames?
- `c9`, `c10g`, `stall` (on request): constant-size MP4, growing file, pause without fill.

## Results

**Works**
- All default tests play with no errors and no corruption.
- Lag ~0.4–0.65 s, steady within a run (differs between runs).
- Skip frames work: the picture freezes exactly on the last frame before a
  pause and goes on with nothing skipped.
- Skips make a 15 fps sender play smoothly at 25; bursts don't show.
- ~10 % unit CPU.

**Doesn't work**
- A 64-bit `mdat` header is refused (`CDemuxMp4` reads 32-bit sizes only).
- A growing CS-MP4 file on the card fails with EIO.
- A pause without gap fill hides the last ~9 frames, then skips them.
- Bigger frames or 30 fps don't cut the lag (the queue counts frames).

Details: [docs/phone-mirroring.md §7.8–7.11](../../docs/phone-mirroring.md).

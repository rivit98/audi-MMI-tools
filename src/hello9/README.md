# hello9

Live H.264 streaming through the DM642 with io-media untouched. Runs the
receiver's code (`src/common/video/hw_stream.h`: CS-MP4 header, own resource
manager, gap fill) fed from a clip on the card, the way a phone would feed
it. Also tests io-media's play speed and an audio track in the stream. C++.

- **Run:** `hello9 <sd_path> [test,test,...]` (default: `c5,spf,spl,spfast,av_aac,av_mp3,cs_aac,cs_mp3,lv_aac,lv_mp3`, ~6 min). Radio on, film with sound.
- **Output:** `var/hello9.log` (`RESULT` lines, lag), `var/hello9/` (io-media queries, slog)

## Verifies (default run)

Built, not yet run on the car.

- `c5`: control, plain MP4 file.
- `spf`: c5 with io-media's play speed (devctl `0x80040F0B`) changed while
  it plays: 2000, 500, then 1000. Does the picture's counter follow?
  Answer: the `+N ms speed` lines (io-media's answer), `t=` lines (TIME
  position against wall time), the film.
- `spl`: an on-time 25 fps live stream at 1100 and 1250. Can a faster
  speed drain io-media's ~9 queued frames (lower `lag`), and what happens
  when its reads catch up with the live edge?
- `spfast`: the sender 10% too fast, so the lag grows; from 15 s a
  controller plays at 1200 while the lag is 150 ms over its level. Answer:
  `lag` in the `t=` lines falls back after each `speed 1000 -> 1200`.
- `av_aac`, `av_mp3`: plain MP4s with sound (a beep with every white flash)
  on `mixer:DSPOUT_TDM1_20_SAD`, the output heard in the car. Does
  io-media play an MP4's audio track in sync with the picture? Answer:
  `RESULT <test> track:` (io-media's track info), the film's sound.
- `cs_aac`, `cs_mp3`: the same in the constant-size A/V layout
  (`media/csav.py`): chunks of video frames plus audio frames, every
  sample of a track the same size. AAC frames are padded with zero bytes,
  which the DSP's decoder may or may not ignore; 48 kHz CBR MP3 needs no
  padding.
- `lv_aac`, `lv_mp3`: `cs_*` served through the stream server a chunk at a
  time on the clip's own clock (`paced.h`), as a live stream with sound
  would come. Answer: the `t=` lines (preads, waits) and the film.

## Verifies (on request, answered)

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

# hello9

The streaming test: the unit's DM642 decoder showing a **live** H.264
stream, with the unit's own io-media left as it is. It works
([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.10). hello9
runs the code `audi_stream_receiver` uses for the phone's frames (`src/common/hw_stream.h`), fed from a clip on the card the
way a phone would feed it. Written in C++
([docs/cpp-apps.md](../../docs/cpp-apps.md)).

| Test | How frames come | Question |
|---|---|---|
| `c5` | plain MP4 on the card (720×336), played as a file | Control: seen on every run. Black here means the display path broke, not the stream |
| `live` | c11 pushed frame by frame, 25 fps, 30 s; the header declares the receiver's default (~1.6 h) | Does the MMI-built stream play (header from the SPS/PPS, frames padded on the MMI, a `moov` declaring 150 000 frames)? Is the lag the same steady ~0.5 s? |
| `gap` | the same, with a 2 s pause at 8 s and a 5 s pause at 16 s; **gap fill on** | Do the skip frames work on the DSP? The picture should freeze on the **last frame before each pause (199, 399)** and go on without skipping |
| `slow` | 15 fps, 20 s, gap fill on | Do skips make a slow sender up to 25 fps? The counter runs at 15 a second, smoothly |
| `burst` | 25 fps, but 5 frames at a time every 200 ms, 20 s | Does io-media cope with clumped arrival (USB)? Lag? |
| `s56` | as `live`, 15 s, every frame padded to twice the size (56 KB) | **Delay tuning:** is io-media's ~0.5 s a queue of bytes (the lag drops) or of frames (it stays)? |
| `s112` | the same, 4× (112 KB) | the same, and what bigger frames cost (the phone's may need 64-256 KB) |
| `f30` | the stream timed at 30 fps, frames pushed at 30 fps, 15 s | If the queue is frames, the lag drops to ~5/6 |
| `c9` | constant-size MP4 file (on request) | Seen (below) |
| `stall` | `gap` without gap fill (on request) | For comparison: freezes ~9 frames early, then skips them (below) |

The default is `c5,live,gap,slow,burst,s56,s112,f30`, about 4.5 minutes. `c10g` (a CS-MP4
growing on the card) was dropped: it fails with EIO (below), and the
resource manager makes it unnecessary.

How each piece works:

- **The clip:** `c11_h264_336.h264` (`media/mkclips.sh`), raw Annex-B as an
  encoder hands it out: 550 frames, an IDR every 25, looped (frame 0 is an
  IDR, so the counter on screen restarts). hello9 splits it into frames
  (`h264::split_access_units`) and takes the SPS/PPS and the frame size S
  (largest frame + filler, in 4 KB steps: 28 KB) from it.
- **The feeder:** a thread pushes frame k when `schedule.h` says it is due
  (pauses, rate, bursts), into `HwStream::push`, as the receiver's USB
  thread pushes the phone's.
- **HwStream** (`src/common/hw_stream.h`): `live::Stream` builds the
  CS-MP4 header (`csmp4::build_header`, byte for byte what
  `media/csmp4.py` writes, checked by `make test`), pads every frame to S
  and keeps the last 128; `StreamServer` serves it at
  `/dev/hello9/live.mp4` with blocking reads; once the first IDR is in,
  `Player` creates the io-media graph and plays it; 1 s later `vout` puts
  the picture on screen (VCAP 0 input 1).
- **Gap fill** (`live::Stream::fill`, run every 10 ms by the server): when
  the stream is 2+ frames behind its frame clock and nothing came for a
  frame period, it appends **skip frames**: non-reference P frames with
  every macroblock skipped, which repeat the picture. ffmpeg decodes them
  as exact copies of the frame before (`make test`, `tests/check_live.py`).
  They never put the stream ahead of the clock.
- **The driver** turns the knob once when the moving picture appears. In
  `gap` and `stall`, they also turn it once more each time the picture
  moves again after a freeze. hello9 logs every knob time.

## The resource manager

`src/common/stream_server.cpp`, one thread, no
patches to anything on the unit:

- **The path** ends in `.mp4`, so io-media picks its MP4 demuxer. io-media
  opens it like any file (`open`/`read`/`lseek`/`fstat`, `pread` also
  handled). `st_size` is the declared size.
- **A read** whose bytes don't exist yet gets no reply until they do. The
  reader blocks, as it would on a live stream. A 10 ms dispatch timeout
  runs the gap fill and re-checks the waiting reads. If io-media gives up
  on a read (unblock) it gets `EINTR`; at the end, waiting reads get EOF.
  A read more than 10 s past the newest frame (a probe of the end) is
  answered at once with filler.
- **Logging:** the server never writes the log itself, because `log.c`
  fsyncs every line. It passes notes to the main thread, which logs the
  first 40 reads in detail and a status line every second.

**Lag** is measured on every io-media TIME event at position p: now,
minus when the frame at p was pushed (`HwStream::lag_ms`). After a pause
without fill io-media's clock runs ahead, so it means nothing there; with
fill the stream stays on its clock, so it should stay meaningful.

**`<sys/pm.h>`:** the resource-manager headers need it and the sysroot
doesn't have it. `src/common/qnx/sys/pm.h` stands in; it covers one type
used only by `_IO_POWER`, which isn't handled.

## Running

- **Run:** `hello9 <sd_path> [test,test,...]`, e.g. `c5,gap,stall`. `make
  sd` puts the clips (c5, c9, c11 from `media/mkclips.sh`) on the card
  when hello9 is in `SD_APPS`.
- **Output:**
  - `hello9.log`: every step and every io-media event. For the live tests:
    the stream (`hw:` line: size, POC type, S, frames declared, header
    size, gap fill), the pauses and the last frame before each, the first
    40 reads, a status line per second (frames pushed, skips, how far
    io-media has read, waits, read-ahead, position, lag). Then `RESULT`
    lines and the summary.
  - `hello9/`: `query_<test>.log` (io-media's view of the graph:
    `GetTrackInfo`, `GetVideoInfo`), `slog.txt` (the system log after the
    tests, for io-media's own messages).
- **Film it:** the phone video settles what the log can't (where the
  picture froze, whether it skipped). Keep the counter in frame.

## Reading the result

`RESULT <test>` lines carry:

| Field | Meaning |
|---|---|
| `playing_ms` | First STATUS "playing" after play |
| `first_time_ms`, `time_events`, `pos_ms` | Progress |
| `errors` | ERROR events, with io-media's `iom_errno` |
| `video`, `WxH` | From the query |
| `knob`, `at_s` | Count, and seconds after play. Counts every ch8 message, so other buttons and the car's power-off messages show up as turns |
| `lag` | min / median / max / last, ms |

`RESULT <test> stream` lines (live tests) add:

| Field | Meaning |
|---|---|
| `reads`, `preads` | Read requests, and how many were `pread`s |
| `read=` | Smallest..largest read size |
| `waited`, `wait_max`, `wait_avg` | Reads that had to wait for their frames, and for how long |
| `lead_max` | Read-ahead: how many frames past the newest one io-media asked for |
| `unblocked` | Waiting reads io-media gave up on |

`RESULT <test> frames` lines (live tests): `pushed` / `refused` by the
feeder (with the first refusal), and the stream's `real`, `idr`, `skips`,
`gaps`, `too_big`, `wait_idr`, `bad`, `max_frame`, `sample` (S),
`evicted_reads`.

| What you see | Meaning |
|---|---|
| c5 black | The display path broke: nothing else counts |
| `live` black, ERROR at open | The MMI-built header differs from the `csmp4.py` stream that plays (below) in something io-media checks. `make test` says it is byte-identical to csmp4.py's for the same frames, so suspect the size of the declaration (150 000 frames): run `gap` (600 s declared) to compare |
| `live` seen, lag ~0.5 s steady | **The receiver's path works on the car.** Same as the `csmp4.py` stream |
| `gap`: froze on 199 and 399, no skip after, knob after each pause | **Skip frames work on the DSP**: a sender that pauses loses nothing |
| `gap`: froze ~9 frames early and skipped, as in `stall` | The skips didn't reach io-media or didn't release its queue: look at `skips` in `RESULT gap frames` and the per-second lines |
| `gap`: garbage or a black picture after a pause | The DSP doesn't like the skip frames (POC type 2 with several non-reference frames in a row is outside the spec). The fix would be to make them reference frames and renumber what follows |
| `slow`: smooth, lag steady | Skips make up a slow sender |
| `burst`: lag steady, no errors | Clumped arrival is fine |
| `s56` / `s112` lag ≈ `live`'s | The queue is frames: bigger frames don't cost delay, so the phone's frame size cap only costs bandwidth |
| `s56` / `s112` lag clearly lower | The queue is bytes: pad frames bigger to cut the delay (at the cost of io-media reading more) |
| `f30` lag ≈ 5/6 of `live`'s | The queue is frames: a higher stream rate cuts the delay |
| `s112` errors or stutter | io-media can't read 112 KB × 25 a second (2.8 MB/s) comfortably: keep the phone's frames smaller |
| errors, `unblocked`, `refused` > 0 | Read the first refusal and the server notes |

## On the car

### The MMI-built stream and gap fill

**All eight default tests play with no errors, no refused frames and no
corruption.** Median lag in ms, from two complete runs, with what a video
of the counter shows (the second run was filmed from start to end):

| test | lag, run A | lag, run B | on video |
|---|---|---|---|
| `live` | 635 | 396 | Counter climbs 25 a second, wraps cleanly |
| `gap` | 593 | 397 | **Freezes on exactly 199 for 2 s and 399 for 5 s**, then goes on with nothing skipped (199 → 203 → 206 → 213 at the 25 fps pace). 176 skip frames in 5 gaps, no garbage |
| `slow` | 595 | 400 | Counter climbs 15 a second, lag steady: 210 skip frames make up the rate |
| `burst` | 620 | 414 | Counter climbs 25 a second: clumped arrival doesn't show |
| `s56` | 515 | 342 | Counter climbs 25 a second |
| `s112` | 657 | 362 | Counter climbs 25 a second at 2.8 MB/s of reads |
| `f30` | 577 | 365 | Counter climbs 30 a second |

- **Skip frames work on the DSP**, even several non-reference frames in a
  row with POC type 2, which is outside the spec. A sender that pauses
  loses nothing, and the last frame before a pause does reach the screen.
- **The io-media queue isn't counted in bytes.** With frames 2× and 4× the
  size, a byte queue would halve and quarter the lag. It moves by −120 to
  +22 ms, and not in order of frame size. So a bigger frame size cap costs
  no delay.
- **30 fps isn't worth it:** the lag drops by 8–9 %, 31–58 ms, less than
  the 1/6 a queue of frames predicts.
- **The lag differs from run to run, and is steady within a run:** every
  test in run A sits ~200 ms above the same test in run B. The cause isn't
  known. Unit CPU load is about the same in both. Compare lag within one
  run only.
- **Power-off doesn't stop it:** the ch8 power sequence (bytes 14–15
  `02 02`, `01 03`, `01 00`, [docs/power-onoff.md](../../docs/power-onoff.md))
  came during run A's `slow`, and the unit played the remaining tests to
  the end.

Nothing is left to answer, so hello9 is not in the default run. `make test`
agrees: the MMI-built header and frames are byte-identical to `csmp4.py`'s,
and ffmpeg decodes a filled gap with every skip an exact copy of the frame
before. A host run of `gap` on c11 freezes on 199 for 49 frames and on 399
for 125, the same picture as on the car.

### The prebuilt stream

The results below come from a CS-MP4 stream prebuilt on the host by
`media/csmp4.py` and served frame by frame by the same resource manager.

| test | result |
|---|---|
| `c5` | **Seen** on every run: 22 TIME events, one knob turn 3.7–5.6 s after play, video 720×336. hello9's own io-media client works |
| `c9` | **Seen**: 22 TIME events, knob 4.4 s, 720×336. First TIME 567 ms (c5: 369). The filler-NAL padding and the constant `stsz` are accepted, by `CDemuxMp4` and by the DSP |
| `c10g` | The header opens (first TIME "0 of 600000 ms", so `mdat` size 0 and a `moov` declaring 10 min are fine), then ERROR "Input/output error" 813 ms after play; the query gets `0x80002`. Black. io-media reads past the end of what was written (1.4 MB head start). A growing file on the card is out, and nothing needs it |
| `live` | **Seen, the full 30 s, no errors**: knob 3.6 s, 76 TIME events, position 30.4 s. 341 reads, all 64 KB, all sequential, **every one waited** (avg 89 ms, max 140 ms), read-ahead at most 107 ms (~2.7 frames), nothing unblocked. **Lag steady at 514–560 ms** (median 543). On video the counter climbs 25 a second and wraps 549 → 0 cleanly |
| `stall` | **Seen, no errors, no unblocked reads.** Both pauses waited out (the 5 s read was answered after 5113 ms). Knob after each pause |

### The `mdat` header must be 32-bit

A CS-MP4 whose `mdat` has a 64-bit header (size field 1) never reaches the
decoder: ERROR "Input/output error" ~40 ms after play, no TIME event, the
query gets `iom_errno 0x80002` (slog: "No metadata available"). io-media
opens the file, reads **64 KB at offset 0 once**, and stops.

The cause is in io-media-nvidia, not in `mp4_parser.so`
([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.9). The
trackplayer's own MP4 demuxer (`CDemuxMp4`) walks the top-level boxes
reading **32-bit sizes only** (`FUN_08225a20`). It takes a 64-bit `mdat`
as a 1-byte box, lands in the middle of the header, and gives up (-8 →
`0x80000018`, so the track never opens). All of that is inside the first
64 KB.

So `csmp4.py` writes `mdat` with an 8-byte header:
- a complete file (c9) gets its exact size.
- a file that declares more than it has (c10, 10 min) gets size 0 ("to the
  end of the file"). io-media also refuses a box that ends past the file's
  current size. For the live stream, `st_size` is the declared total, so
  `mdat` ends there.

The stream must stay under 4 GB, ~1.6 h at 28 KB × 25 fps.
`stream_plan.cpp` refuses a header io-media would refuse: a 64-bit
`mdat`, `mdat` not right after `moov`, an `mdat` size that isn't 0 or
the declared end, or 4 GB or more. The same walk as io-media's, run on
the clips, passes c9, c10 at the growing copy's 2 s head start and the
live stream's declared size. ffmpeg decodes all 550 frames of both.

### A pause without gap fill (`stall`)

What a video of the counter shows (10 fps stills):

- The picture **freezes for the length of the pause**: on 190 for 2.0 s,
  on 391 for 5.2 s. The server had served up to frame 198/199 and 399, so
  the display stops **~9 frames (~0.35 s) behind** the newest frame io-media
  has. That matches the 0.5 s lag.
- When frames come again it **skips about 10 frames** (190 → 202,
  391 → 401) and plays on at 25 fps. Latency doesn't build up over a gap,
  but the frames that were queued when it ran dry are never shown.
- io-media's clock keeps running through a pause: TIME positions carry on
  past the last frame it has, so the `lag` figures after a pause (−1.4 s,
  then −6.4 s) are pause length plus normal lag, not a measurement. Judge
  stalls from a video.

### CPU

The MMI's own `CPU:` line in the slog: 20–23 % during `live`, 12–13 %
idle afterwards. The hardware path plus the server costs about 10 %.

### Conclusions

- **Live hardware H.264 works** with io-media untouched: CS-MP4 (32-bit
  `mdat`, constant `stsz`, filler-NAL padding, over-declared `moov`,
  `st_size` = declared total) served by our own resource manager whose
  reads block until the frame exists.
- The latency io-media adds is ~0.5 s, and steady.
- The stream must **not go quiet when nothing changes**: the last ~9 frames
  before a gap stay hidden until the next frame arrives, and are then
  dropped. The sender has to keep frames coming (repeat the last frame at
  a steady rate), or the final state of a screen change isn't shown. That
  is what gap fill is for.

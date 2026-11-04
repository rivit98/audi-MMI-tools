# hello9

The streaming test: can the unit's DM642 decoder show a **live** H.264
stream, with the unit's own io-media left as it is? It runs the tests that
came out of the reverse engineering of io-media
([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.8), which
moved here from hello6. Written in C++
([docs/cpp-apps.md](../../docs/cpp-apps.md)).

| Test | Source | Question |
|---|---|---|
| `c5` | plain MP4 on the card (720×336) | Control: seen on every hello6 run since 21.10. Black here means the display path broke, not the stream |
| `c9` | constant-sample-size MP4 (CS-MP4) on the card | Does the DSP take frames padded to one size with a filler NAL? |
| `c10g` | CS-MP4 whose header declares 10 min, written to the card while it plays (2 s head start) | Does io-media start on a file that's still growing, and keep up? |
| `live` | **hello9 serves the stream itself** at `/dev/hello9/live.mp4`, 30 s | Does io-media play a stream whose reads wait for the frames? How far ahead does it read? What is the lag? |
| `stall` | the same, with a 2 s pause at 8 s and a 5 s pause at 16 s | Does io-media wait out a gap in the stream, and catch up? |

How each piece works:

- **Player:** each test plays through hello9's own io-media client
  (`common/iom.c`, hello5's, the calls `testapp-file` makes). Every io-media
  event is logged.
- **Picture:** a layer-manager video displayable on VCAP 0, with VCAP 0
  switched to input 1 (the DSP's video port) through dev-videoctrl
  (`vout.cpp`). This is hello6's working path; VCAP 1 isn't tried.
- **The driver** turns the knob once when the moving picture appears. In
  the `stall` test, they also turn it once more each time the picture moves
  again after a freeze. hello9 logs every knob time.

## The live stream

`stream_server.cpp` is a QNX resource manager, with one thread and no
patches to anything on the unit:

- **The path** ends in `.mp4`, so io-media's `iom.cfg` picks `mp4_parser`.
  io-media opens it like any file (`open`/`read`/`lseek`/`fstat`, `pread`
  also handled).
- **The header** comes from c10 and is always available. It declares 10
  minutes of 28 KB frames, and `st_size` is the declared size (430 MB).
- **Frame k** is released k/25 s after the stream starts (plus any pauses
  before it). It's read from c10 on the card, and the clip's 550 frames
  loop. The loop point is a keyframe, so the counter on screen restarts.
- **A read** whose bytes aren't released yet gets no reply until they are.
  The reader blocks, as it would on a live stream. A 10 ms dispatch
  timeout re-checks the waiting reads. If io-media gives up on a read
  (unblock) it gets `EINTR`; at the end of a test, waiting reads get EOF.
- **Logging:** the server never writes the log itself, because `log.c`
  fsyncs every line. It passes notes to the main thread, which logs the
  first 40 reads in detail and a status line every second.

The pure part (header parsing, release schedule, byte → source mapping)
is `stream_plan.cpp`, tested on the build host by `tests/test_hello9.cpp`
(`make test`).

**Lag** is measured on every io-media TIME event at position p: wall time
since the stream started, minus the release time of the frame at p. That
is how long after a frame "arrived" io-media says it plays it, which is
the latency floor this path adds before the phone's encoder and the USB.

**`<sys/pm.h>`:** the resource-manager headers need it and the sysroot
doesn't have it. `src/common/qnx/sys/pm.h` stands in; it covers one type
used only by `_IO_POWER`, which hello9 doesn't handle.

## Running

- **Run:** `hello9 <sd_path> [test,test,...]`. The default is all five, in
  the order above, about 3.5 minutes. `make sd` puts the clips (from
  `media/mkclips.sh`) on the card when hello9 is in `SD_APPS`.
- **Output:**
  - `hello9.log`: every step and every io-media event. For the live tests:
    the stream's numbers, the first 40 reads, a status line per second
    (released frames, how far io-media has read, waits, read-ahead,
    position, lag). Then `RESULT` lines and the summary.
  - `hello9/`: `query_<test>.log` (io-media's view of the graph:
    `GetTrackInfo`, `GetVideoInfo`), `slog.txt` (the system log after the
    tests, for io-media's own messages), `grow_c10.mp4`.

## Reading the result

`RESULT <test>` lines carry:

| Field | Meaning |
|---|---|
| `playing_ms` | First STATUS "playing" after play |
| `first_time_ms`, `time_events`, `pos_ms` | Progress |
| `errors` | ERROR events, with io-media's `iom_errno` |
| `video`, `WxH` | From the query |
| `knob`, `at_s` | Count, and seconds after play |
| `lag` | min / median / max / last, ms |

`RESULT <test> stream` lines (live tests) add:

| Field | Meaning |
|---|---|
| `reads`, `preads` | Read requests, and how many were `pread`s |
| `read=` | Smallest..largest read size |
| `waited`, `wait_max`, `wait_avg` | Reads that had to wait for their frames, and for how long |
| `lead_max` | Read-ahead: how far past the newest released frame io-media asked |
| `unblocked` | Waiting reads io-media gave up on |

| What you see | Meaning |
|---|---|
| c5 black | The display path broke: nothing else counts |
| c9 still ERROR within ~40 ms, no TIME | Something else in the header is refused at open. Next suspect: the constant `stsz` size. Try an all-equal per-sample table (docs/phone-mirroring.md §7.9) |
| c9 black, c5 seen | The filler NAL padding: pad inside the last slice instead |
| c10g black, live seen | io-media read past the end of the growing file. Live streaming works through the resource manager |
| `live` seen, lag steady | **Hardware streaming works.** The lag is the latency floor. Next: frames from the phone over AOA |
| `live` seen, lag grows | io-media's clock runs ahead of the stream, or it buffers more and more |
| `stall`: knob after each pause, lag back to normal | io-media waits out gaps |
| `stall`: no knob after a pause, errors, or `unblocked` | io-media gives up on slow reads. The MMI must fill gaps itself (repeat-frame P-frames) |
| `stall`: `lag` negative after a pause | Expected (04.11): io-media's clock runs on through a gap, so TIME positions run ahead of the stream. Use the phone video for stalls |

## On the car

**2026-11-03** (all five tests, 173 s, after hello8; `runs/3.11.2026 -
run1/`, phone video `VID_20261103_103533` from c5 on):

| test | result |
|---|---|
| `c5` | **Seen**: 22 TIME events, one knob turn 5.6 s after play, video 720×336. The display path and hello9's own io-media client work |
| `c9` | ERROR "Input/output error" 43 ms after play, no TIME event; the query gets `iom_errno 0x80002` (slog: "No metadata available"). Black |
| `c10g` | The same, 41 ms after play |
| `live` | io-media opened `/dev/hello9/live.mp4` three times, read **64 KB at offset 0 once** (waited 74 ms for frame 2), then the same ERROR. No further reads; the server ran its 30 s idle |
| `stall` | The same as `live` (one read, 62 ms wait) |

Nothing reached the decoder: every CS-MP4 failed while io-media was
opening the file. The cause was found afterwards in io-media-nvidia, not
in `mp4_parser.so` ([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.9).
The trackplayer's own MP4 demuxer (`CDemuxMp4`) walks the top-level boxes
reading **32-bit sizes only** (`FUN_08225a20`). `csmp4.py` wrote `mdat`
with a 64-bit header (size field 1). The walk took that as a 1-byte box,
landed in the middle of the header, and gave up (-8 → `0x80000018`, so the
track never opened). All of that is inside the first 64 KB, which is
exactly the one read `live` saw. The filler-NAL padding, the constant
`stsz` and the over-declared `moov` were never tested.

**Fixed the same day; needs the next run.** `csmp4.py` now writes `mdat`
with an 8-byte header and nothing else changed:
- c9 gets its exact size.
- c10, which declares 10 min, gets size 0 ("to the end of the file").
  That is the only form the growing copy can pass with: io-media also
  refuses a box that ends past the file's current size. For the live
  stream, `st_size` is the declared total, so `mdat` ends there.

The stream must stay under 4 GB, ~1.6 h at 28 KB × 25 fps.
`stream_plan.cpp` now refuses a header io-media would refuse: a 64-bit
`mdat`, `mdat` not right after `moov`, an `mdat` size that isn't 0 or
the declared end, or 4 GB or more. The same walk as io-media's, run on
the new clips, passes c9, c10 at the growing copy's 2 s head start and
the live stream's declared size. ffmpeg decodes all 550 frames of both.

**2026-11-04** (hello5 `t`, then all five tests, 174 s, no FAIL lines;
`runs/4.11.2026 - streaming/`; phone video `VID_20261104_164346`
covers the whole of hello9, `_v2` is the `live` test cut out):

| test | result |
|---|---|
| `c5` | **Seen**, knob 3.7 s, as on every run |
| `c9` | **Seen**: 22 TIME events, knob 4.4 s, 720×336. First TIME 567 ms (c5: 369). The filler-NAL padding and the constant `stsz` are accepted, by `CDemuxMp4` and by the DSP |
| `c10g` | The header opens (first TIME "0 of 600000 ms", so `mdat` size 0 and a `moov` declaring 10 min are fine), then ERROR "Input/output error" 813 ms after play; the query gets `0x80002`. Black. io-media read past the end of what was written (1.4 MB head start). A growing file on the card is out, and nothing needs it |
| `live` | **Seen, the full 30 s, no errors**: knob 3.6 s, 76 TIME events, position 30.4 s. 341 reads, all 64 KB, all sequential, **every one waited** (avg 89 ms, max 140 ms), read-ahead at most 107 ms (~2.7 frames), nothing unblocked. **Lag steady at 514–560 ms** (median 543). Phone video: the counter climbs 25 a second and wraps 549 → 0 cleanly |
| `stall` | **Seen, no errors, no unblocked reads.** Both pauses waited out (the 5 s read was answered after 5113 ms). Knob at 3.6, 11.9, 24.5 s (after each pause) and 34.3 s (unexplained extra turn) |

What the phone video shows in `stall` (10 fps stills of the counter):

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
  stalls from the video.

CPU (the MMI's own `CPU:` line in the slog): 20–23 % during `live`, 12–13 %
idle afterwards. The hardware path plus the server costs about 10 %.

**Conclusions:**

- **Live hardware H.264 works** with io-media untouched: CS-MP4 (32-bit
  `mdat`, constant `stsz`, filler-NAL padding, over-declared `moov`,
  `st_size` = declared total) served by our own resource manager whose
  reads block until the frame exists.
- The latency io-media adds is ~0.5 s, and steady.
- The stream must **not go quiet when nothing changes**: the last ~9 frames
  before a gap stay hidden until the next frame arrives, and are then
  dropped. The sender has to keep frames coming (repeat the last frame at
  a steady rate), or the final state of a screen change isn't shown.
- `c10g` has answered its question and can be dropped from the default
  list. Next steps: [docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.10.

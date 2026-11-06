# audi_stream_receiver

The phone's screen on the MMI, over USB. An Android phone app
(`android/`, the Audi streaming APK) captures its screen and sends it as JPEG
or PNG pictures, a few a second, over Android Open Accessory (AOA). This app
receives them, decodes them on the CPU and shows them full screen.

*Renamed from `usb_tx_test` on 2026-11-03 (phone app: `mmi3g.usbtx` -> Audi streaming, `mmi3g.audistreaming`). Car logs before that date are `usb_tx_test.log` / `usb_tx_perf.csv`.*

```
phone: android/ app  --USB (AOA bulk)-->  MMI: audi_stream_receiver  -->  screen + audi_stream_receiver.log
```

- **Run:** `audi_stream_receiver <sd_path>`. It runs until the SD card is pulled or
  it gets SIGTERM/INT/HUP, so it is always the last app of a run.
- **Output:** `audi_stream_receiver.log` on the card (every USBDI result, state
  change, frame and picture; a `cpu:` line every 10 s with the app's own
  and the unit's CPU load, per thread, ` - HIGH` at 50 % own load). A failed handshake or a removal also saves
  the system log to `var/usb_slog_<n>.txt`. The previous run's files are
  kept as `audi_stream_receiver.prev.log` and `var/usb_slog_prev_<n>.txt`.
- **Build:** `make` (MMI side), `make apk` (phone app), `make test` and
  `make apk-test` (protocol, pictures, JPEG/PNG, downscale), `make aoa-host` (desk test:
  the PC plays the MMI).

How the USB side works, what the unit's USB stack does and the full
protocol are in the main [README](../../README.md#audi_stream_receiver-make-sd-sd_appsaudi_stream_receiver)
and [docs/usb-stack.md](../../docs/usb-stack.md). The frames are in
`proto.h` (= `android/.../Proto.kt`).

## Using it

1. Install the APK on the phone. Insert the SD card. The MMI says
   *Waiting for phone*.
2. Plug the phone into the car's USB port. The status bar goes amber
   (*switching*), then blue. The phone asks to open Audi streaming for this
   accessory: accept, and the bar turns green, *Connected*.
3. On the phone, tap **Stream screen** and allow the screen capture. The
   phone's screen appears on the MMI: landscape on the whole screen,
   portrait on the left half with the status panel beside it. The
   phone's *Details on the MMI* switch adds a slim status bar over a
   landscape picture. *Format* on the phone picks JPEG (fast) or PNG
   (sharp text, bigger). (A third, *PNG when still*, was dropped on
   2026-11-04.)
4. Use the phone as usual. Back, Home, other apps, turning the phone,
   unplugging and replugging all keep the stream going. Stop it in the
   app or from its notification. Pull the SD card to end the MMI side.

## Hardware H.264 (built 2026-11-04, not yet run on the car)

Besides JPEG / PNG pictures decoded on the CPU, the app takes an **H.264
stream** and has the unit's DM642 decode it: no CPU decoding, 25 fps,
~0.5 s added by io-media ([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.10).
On the phone: *Format* → **H.264** (Android 10+). The phone encodes its
screen in hardware at 720×432 (the MMI's 720×576 limit, the screen's 5:3
shape) at the *Max fps* slider's rate (up to 25); the MMI's stream runs at
25 fps and fills the rest with skip frames.

- **Protocol** (`proto.h`): `VIDEO_CAPS` sets bit 2 (H.264) and appends the
  limits (720×576, 25 fps, frames up to 256 KB). The phone sends
  `H264_CONFIG` (0x0B: fps, the largest frame, SPS, PPS), then its frames
  as `VIDEO_START` / `VIDEO_DATA` with format 3 (Annex-B, one frame each,
  acked at once). The MMI answers `H264_STATUS` (0x0C): `PLAYING`, `SYNC`
  (send an IDR), `REFUSED` (fall back to pictures), `CONFIG` (send the
  config again).
- **`hwvideo.cpp`** checks the config (Baseline, level ≤ 3.0, ≤ 720×576,
  `live::check_decodable`), then runs `src/common/hw_stream.h` on its own
  thread: the CS-MP4 header from the phone's SPS/PPS, frames padded to the
  config's size (in 4 KB steps), a resource manager at
  `/dev/audi_stream/live.mp4`, the io-media graph once the first IDR is
  in, the picture 1 s later. Gaps are filled with skip frames. A new
  config restarts the stream; the same config again changes nothing. The
  stream ends on `VIDEO_STOP`, a JPEG / PNG picture, unplugging, or when
  its declared frames run out (the 4 GB limit: ~1.6 h at 28 KB frames,
  ~45 min at 64 KB; then it starts again and asks for an IDR). The last
  8 MB of frames are kept for io-media (16-128 frames).
- **The rear camera shares VCAP 0** (docs/rear-camera.md §9): an app that
  sets VP1, or disables it, while the camera is up hides the camera from
  the driver, and nothing tells an app whether it is up. So
  `/dev/videoctrl` is touched only when the user starts or stops
  streaming. A restart (new config, stream full) keeps the picture and
  VCAP as they are and swaps only the io-media graph and the server.
  **When io-media reports an error** (perhaps the HMI took the video for
  its media or the camera), the stream ends without touching
  `/dev/videoctrl`, and the phone is told `REFUSED` (pictures for the rest
  of the connection). If no new stream follows a restart within 5 s, the
  CPU-drawn screen comes back.
- **Still a risk:** starting H.264 *while reversing* switches VCAP 0 to
  the DSP (the camera picture goes until the next camera session), and
  stopping it while reversing disables VCAP 0. Don't start or stop
  streaming in reverse. Reopening the CPU-drawn screen shows displayables
  {40, 16, 19}; whether that covers the camera's (0x11) is a question for
  every app, not only this one.
- **The phone side** (`android/`, `CaptureService.kt`, `HwEncoder.kt`):
  MediaCodec, Baseline 3.0, 2.5 Mbit/s CBR, an IDR only at the start and
  when the MMI asks (`I_FRAME_INTERVAL` 10 s); frames are never dropped
  on their own (the ones after would be garbage): a frame bigger than the
  config's `frame_bytes` (64 KB at first) raises it (a new config, the MMI
  restarts its stream, ~1-2 s black), and a queue of over 25 unacked
  frames is thrown away up to the next IDR. If the encoder puts other
  SPS/PPS before its IDRs than in its codec config, those become the
  config. The phone app's status line shows rate, frame sizes and drops.
- **Desk test** (`make aoa-host`): the PC plays the MMI, checks every frame
  as the MMI would and saves the stream to `build/aoa-host/stream.h264`
  (`ffplay` it).
- **Latency:** the phone app's *Latency clock* switch shows a big
  seconds.milliseconds clock. Stream with it on screen and take one photo
  of the phone and the MMI together: the difference is the delay.
- **The screen:** while the stream runs, the main loop closes the
  CPU-drawn screen (the video displayable replaces it) and opens it again
  afterwards.
- **Log:** `h264:` lines: the config, the stream's start and end with its
  totals, refused frames (the first 5), and every 10 s frames in, skips,
  io-media's reads and the lag.
- **Tested so far:** on the build host (`make test`: header, padding,
  push / fill / read, skips decoded by ffmpeg; `make apk-test`: the
  phone's protocol and H.264 helpers), and the same MMI code with a clip
  instead of the phone in hello9 (`src/hello9/README.md`). The C++ code
  makes this a C++ app since 04.11 (`docs/cpp-apps.md`); its `.c` files
  are still C, and JPEG / PNG streaming works as before (confirmed by the
  user).

## Text messages: dropped (2026-10-25)

Until 2026-10-25 the phone app could also send **typed text**. Each line
went as a `TEXT` frame (`seq u16` + UTF-8), the MMI showed it in a list
(and as a caption over a landscape picture for 15 s) and answered `ACK`,
and the phone ticked it off. It was the first thing that worked over the
link (2026-10-21) and proved the USB path end to end.

It was removed because it won't be needed: what the link is for is the
phone's screen. Removed on both sides: the phone's text field, Send
button and sent list; `TEXT`/`ACK` in `Proto.kt`, `proto.h` and
`tools/aoa_host.py`; the MMI's message list and caption (`model.c`,
`ui.c`). The MMI's screen without a picture now says what to do next
instead.

Frame types `0x02` (`TEXT`) and `0x03` (`ACK`) are **retired, not free**:
an old phone app that still sends `TEXT` gets `rx unknown frame type 0x02`
in the log and nothing else. Don't reuse the numbers.

## On the car

**2026-10-21:** the USB path works end to end. A Redmi Note 8 Pro in MTP
mode was switched into accessory mode, said `HELLO`, and three texts were
received and shown. On the first connection the phone needs two AOA
`START`s before it comes back as an accessory. An accessory session that
nobody opens on the phone drops after ~18 s, and the app reconnects by
itself. The 12.10–19.10 runs failed on `DEV_NOANSWER` for EP0 requests with
the same phone (docs/usb-stack.md).

**2026-10-25:** first run with screen streaming. **No picture reached the
MMI.**

- The USB side was fine again: AOA handshake on the 2nd insertion
  (`GET_PROTOCOL` version 2, `START`), accessory `18d1:2d00` with endpoints
  IN `0x81` / OUT `0x01`, `HELLO` and `HELLO_REPLY`. One `TEXT` ("E")
  arrived and was shown.
- The phone showed *Stop stream* (capture running), but its status said
  *Video: waiting for the MMI; sent 0*, while its header said *Connected
  to MMI*. The MMI never got a `VIDEO_START`.
- The MMI log has a new `HELLO` from the phone at +25 s, +56 s and +68 s
  with no USB removal in between, each soon after the phone was turned or
  the screen-capture prompt was answered (phone video).
- **Cause:** turning the phone recreated the app's activity. The old
  activity closed its link and the new one opened another (the new
  `HELLO`s). The old activity's `onClosed` callback arrived after that
  and cleared the shared `Streaming.link`, so the capture service waited
  for an MMI that was connected all along.
- **Fixed** (not yet run on the car): the activity is no longer recreated
  on rotation (`configChanges` in the manifest); `onClosed` from an old
  link is ignored; the activity only ever clears the link it set itself.

**2026-10-28:** streaming works, turning the phone included. 79
pictures shown, 0 bad, 0 dropped. Landscape 800×368 changed to portrait
220×480 mid-stream with no errors, at a steady 1.9–2.0 fps (the phone's
fixed 500 ms tick then). Per picture: decode 55–65 ms (landscape) / 22 ms
(portrait), present ~200 ms whatever the size. The full-screen VFB copy
(~160 ms) is most of that. While streaming, the unit's CPU (slog) went
from ~25 % to 40–60 %.

Changed after it (not yet run on the car): landscape on the whole screen
with *Details on the MMI* (`VIDEO_DETAILS`); *Max fps* slider (default 5)
instead of the 500 ms tick; two pictures in flight; the main loop waits
on a condition variable instead of sleeping 10/50 ms; changed-span VFB
copy (`copy=changed`); perf records (`perf: window` / `perf: stream`
lines, `audi_stream_perf.csv`).

**2026-10-30:** all of the 28.10 changes work. 166 pictures in 54 s,
0 bad, 0 dropped; portrait 220×480, then landscape 800×368; *Details*
on/off worked. Avg 3.0 fps (2.8–3.5 per 10 s window), latency 95 ms
(max 285). Per landscape picture: decode 56 ms, render 23, copy 18–19
(changed spans, ~30 KB instead of 1.5 MB), flip 3: ~100 ms, so the MMI
could do ~10 fps. Main loop busy ~30 %, process CPU 27 % (40–60 % unit
CPU on 28.10). The gaps between pictures were 92–668 ms, so the phone
(the fps slider, a screen that didn't change, or the phone's capture
and compress) set the rate. The end was a clean `VIDEO_STOP`, then the
unplug.

**The text was blurred.** The MMI copies the picture 1:1 (ARGB, no
scaling), so it came blurred from the phone: the capture had the
picture's own size, and Android's mirroring scaled the 2340×1080 screen
down 2.9:1 (landscape) / 4.9:1 (portrait) in one bilinear step, which
skips most of a thin stroke's pixels. JPEG q60 added ringing. Changed
after it (not yet run on the car):

- the capture runs at k× the picture (2× landscape, 4× portrait on this
  phone) and the phone averages each k×k block (`Downscale.kt`); JPEG
  q90;
- PNG (`PROTO_FMT_PNG` 2): the MMI decodes it (`imgdec.c`, stb_image) and
  says so in `VIDEO_CAPS`; the phone's *Format* drop-down picks JPEG
  or PNG (a "PNG when still" choice existed until 2026-11-04);
- `VIDEO_START` carries the phone's downscale + encode time; `perf:`
  lines split size / decode / phone time by format, the CSV has `fmt`
  and `phone_ms` columns.

**Next run:** landscape, a screen with small text (settings, a chat, a
web page). Each *Format* for ~30 s: hold still for a while, then scroll.
Compare text sharpness (photo of the MMI screen) and per format, from
the `perf:` lines: KB, MMI `decode` and `phone` time, and fps. And the
first `cpu:` lines: own load idle, then while streaming each format
(`main` = decode + draw, `usbd` = USB receive), and whether `unit` gets
near 100 %. PNG costs
maybe 2–10× the bytes and slower encodes on the phone, and its decode on
the SH4 has never been measured.

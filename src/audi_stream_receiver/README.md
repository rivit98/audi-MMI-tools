# audi_stream_receiver

The phone's screen on the MMI, over USB. An Android phone app
(`android/`, the Audi streaming APK) captures its screen and sends it over Android Open Accessory (AOA): as
[hardware H.264](#hardware-h264) by default, or as JPEG pictures, a few a
second. This app receives it, decodes JPEG on the CPU and shows it full screen.

```
phone: android/ app  --USB (AOA bulk)-->  MMI: audi_stream_receiver  -->  screen + audi_stream_receiver.log
```

- **Run:** `audi_stream_receiver <sd_path>`. It runs until the SD card is pulled or
  it gets SIGTERM/INT/HUP, so it is always the last app of a run.
- **Output:** `audi_stream_receiver.log` in `var/` on the card (every USBDI result, state
  change, frame and picture; a `cpu:` line every 10 s with the app's own
  and the unit's CPU load, per thread, ` - HIGH` at 50 % own load). A failed handshake or a removal also saves
  the system log to `var/usb_slog_<n>.txt`. The previous run's files are
  kept as `audi_stream_receiver.prev.log` and `var/usb_slog_prev_<n>.txt`.
- **Build:**

  ```sh
  make sd SD_APPS=audi_stream_receiver   # MMI side only (the default run has it last)
  make test                 # unit tests: protocol, pictures, JPEG, UTF-8, wrap, drawing
  make apk                  # android/out/audi_streaming.apk (= make -C android)
  make apk-test             # Kotlin unit tests
  make apk-install          # adb install -r, adb runs in the container
  make aoa-host             # desk test: the PC plays the MMI, shows the stream in an 800x480
                            # window; the newest picture lands in build/aoa-host/latest.jpg,
                            # an H.264 stream in stream.h264
  ```

  The phone app builds in its own image (`mmi-android`), the desk test in
  `mmi-aoa-host`. For `apk-install` and `aoa-host`, stop a host adb server
  first (`adb kill-server`): only one program can own the phone's USB
  interface. The APK is signed with a fixed key in `android/keystore/`, so
  reinstalls keep working after `make clean`.

What the unit's USB stack does, and what that forces on a client:
[docs/usb-stack.md](../../docs/usb-stack.md).

## How it works

**USB.** `audi_stream_receiver` connects to io-usb (`-lusbdi`) with a
wildcard filter and does the
[Android Open Accessory](https://source.android.com/docs/core/interaction/accessories/aoa)
handshake with any new device (vendor requests 51/52/53). An Android phone
re-enumerates as `18d1:2d00`/`2d01` with a bulk IN/OUT interface (`ff/ff`),
no root or USB debugging needed. Android starts the phone app because its
`accessory_filter.xml` matches manufacturer `mmi3g` / model
`audi_stream_receiver`.

**Protocol** (`proto.h` = `android/.../Proto.kt`): frames of
`type u8 | len u16 LE | payload`, max 2048 B. `HELLO`/`HELLO_REPLY` say who
is at each end. The MMI sends `VIDEO_CAPS` with them: a landscape box
(800×480, the whole screen), a portrait box (400×480, the left half),
512 KB max per picture, and the formats it takes (bit 0 JPEG, bit 2 H.264;
bit 1 was PNG, retired, not to be reused). A phone streams only to an MMI that sent it. A picture is
`VIDEO_START` (id, w, h, format, size, the phone's downscale + encode ms)
plus `VIDEO_DATA` chunks (id + ≤2046 B); the MMI answers `VIDEO_ACKS` (how
many more pictures are shown / bad / dropped). The phone counts what it has
in flight down by their sum, in no particular order: it keeps no ids. If
nothing comes back for 3 s with pictures in flight, it starts counting from
zero. `VIDEO_STOP` (or the link going down) brings back the status screen. `VIDEO_DETAILS` (`show u8`) switches the status bar
over a landscape picture; the phone sends it on every connection, and the
MMI starts with it off. `0x02`/`0x03`/`0x08` are retired (below).

**Pictures, phone side** (`CaptureService`, a MediaProjection foreground
service). The phone mirrors its screen into a virtual display at **k
times** the picture that fits the box (`VideoFit.factor`: the largest
k ≤ 4 that fits the screen; a 1080×2340 phone captures 1600×736 for
800×368, 880×1920 for 220×480), resized when the phone turns. When fewer
than two pictures are in flight and 1/*Max fps* (slider 1–15, default 5)
has passed, it takes the newest image, averages each k×k block into one
pixel (`Downscale.box`), compresses it as JPEG q90 and sends it. A static screen sends
nothing; after a replug the new link gets the last picture again. The k×
capture is there because Android's own one-step bilinear scale skips most
of a thin text stroke past 2:1 (blurred text, see [Results](#results-on-the-car)).
The activity is not recreated when the phone turns (`configChanges` in the
manifest), and it only ever clears the link it set itself: when it was
recreated, the old activity's late `onClosed` cleared the new shared link,
and the capture service waited for an MMI that was connected all along.

**Pictures, MMI side.** It reassembles the picture (`vasm.h`), decodes it
on the main thread (`video.cpp`, stb_image), draws it and acks it at once. A picture
stays in flight until its ack. With two in flight the next
picture is already waiting when the current one is shown; only the newest
waiting one is kept, so a slow MMI lowers the rate instead of queueing.
The picture keeps the phone's shape, never rotated or stretched:

- **Landscape** (w ≥ h): centred on the whole screen (800×368 for a 19.5:9
  phone). *Details on the MMI* draws a 28 px status bar over the top
  (state, fps, decode and copy times).
- **Portrait**: full height on the left (220×480), the status panel in the
  remaining ~580 px.

CPU writes into the VFB run at ~9 MB/s, so a full copy costs ~160 ms. So
`screen.c` keeps a RAM copy of each VFB surface and writes only the
changed stretch of each row (`copy=changed`, the default; a full copy
every 30 s and for a new surface). `audi_stream_receiver <sd> copy=full`
(arguments in `sd/apps.conf`) brings back the full copy, to compare.

**Robustness.** io-usb callbacks only queue events; a manager thread does
the work. Unplugging, a transfer error, or a device that isn't Android
(USB stick, iPod) never ends the app: it goes back to *Waiting for phone*.
A link that breaks while the phone stays plugged in is reopened on the
same device, up to 5 tries. `usbd_connect` is retried every 2 s if io-usb
isn't there. It stops only on SD removal or SIGTERM/INT/HUP (no
watchdog), then gives the screen back.

## Logs: perf and CPU

Every log line is fsynced to the card, so the threads that move pictures
only put records into a RAM ring (`perf.cpp`), and a low-priority thread
writes:

- `perf: window` every 10 s of streaming and `perf: stream` when it stops:
  pictures shown, fps, dropped, bad; avg/max ms of `rx` (USB), `wait`,
  `decode`, `render`, `copy` (into the VFB), `flip` and `latency`
  (`VIDEO_START` in to on screen); the JPEG count, KB, MMI decode
  and phone time; VFB KB per picture, main loop busy %, process CPU %.
- `audi_stream_perf.csv`, one line per picture with the same fields in µs
  (`t_ms,id,status,w,h,bytes,rx_us,wait_us,decode_us,render_us,copy_us,flip_us,vfb_bytes,vfb_rows,vfb_full,latency_us,phone_ms`),
  128 lines per fsync.

The first three pictures of a stream are also logged one by one. With a
1 ms clock tick (`perf: clock resolution`), single µs values are good to
±1 ms; averages are fine.

`src/common/cpumon.c` (always on) reads once a second the app's own CPU
time, each thread's, and the kernel idle thread's (procnto pid 1), and
every 10 s logs

```
cpu: 10.0 s: self 27% (max 41%), unit 55% (max 90%), others 28% | main 18%, usbd 6%, usb 2%, perf 1%
```

`others` is the rest of the unit (navigation, media, HMI). `usbd` is
io-usb's callbacks (USB receive), `usb` the manager thread; threads under
0.5 % are left out. A window with own load ≥ 50 % ends in ` - HIGH`. The
screen footer shows the last second.

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
   landscape picture. *Format* on the phone picks H.264 (the
   default) or JPEG.
4. Use the phone as usual. Back, Home, other apps, turning the phone,
   unplugging and replugging all keep the stream going. Stop it in the
   app or from its notification. Pull the SD card to end the MMI side.

## Hardware H.264

Besides JPEG pictures decoded on the CPU, the app takes an **H.264
stream** and has the unit's DM642 decode it: no CPU decoding, ~25 fps,
0.4–0.65 s added by io-media ([docs/phone-mirroring.md](../../docs/phone-mirroring.md) §7.10).
On the phone: *Format* **H.264**, the default (Android 10+). The phone encodes its
screen in hardware at 720×432 (the MMI's 720×576 limit, the screen's 5:3
shape) at the *Max fps* slider's rate (up to 25); the MMI's stream runs at
25 fps and fills the rest with skip frames.

- **Protocol** (`proto.h`): `VIDEO_CAPS` sets bit 2 (H.264) and appends the
  limits (720×576, 25 fps, frames up to 256 KB). The phone sends
  `H264_CONFIG` (0x0B: fps, the largest frame, SPS, PPS, VCAP 0's field
  mode), then its frames
  as `VIDEO_START` / `VIDEO_DATA` with format 3 (Annex-B, one frame each).
  `VIDEO_ACKS` counts them as they enter the stream, every 5 frames or
  200 ms (`ack_batch.h`), well under the phone's 12 in flight. The MMI answers `H264_STATUS` (0x0C): `PLAYING`, `SYNC`
  (send an IDR), `REFUSED` (fall back to pictures), `CONFIG` (send the
  config again).
- **`hwvideo.cpp`** checks the config (Baseline, level ≤ 3.0, ≤ 720×576,
  `live::check_decodable`), then runs `src/common/hw_stream.h` on its own
  thread: the CS-MP4 header from the phone's SPS/PPS, frames padded to the
  config's size (in 4 KB steps), a resource manager at
  `/dev/audi_stream/live.mp4`, the io-media graph once the first IDR is
  in, the picture 1 s later. Gaps are filled with skip frames. A stream
  more than 500 ms ahead of its frame clock (a phone faster than 25 fps, a
  burst after a stall) drops frames until it is on time, then goes on from
  an IDR (`live_stream.h`, clock cap): the lag can't grow. A new
  config restarts the stream; the same config again changes nothing. The
  stream ends on `VIDEO_STOP`, a JPEG picture, unplugging, or when
  its declared frames run out (the 4 GB limit: ~1.6 h at 28 KB frames,
  ~45 min at 64 KB; then it starts again and asks for an IDR). The last
  8 MB of frames are kept for io-media (16-128 frames).
- **The rear camera shares VCAP 0** (docs/rear-camera.md §9): an app that
  sets VP1, or disables it, while the camera is up hides the camera from
  the driver. `common/camera.cpp` follows the camera from dev-videoctrl's
  slog lines. While it is up the app writes nothing to VP1 and shows nothing,
  and the stream runs on unseen. A second after it ends the picture comes
  back (the CPU-drawn screen too), and if the camera starts just then it gets
  its path back. Otherwise `/dev/videoctrl` is touched only when the user
  starts or stops streaming. A restart (new config, stream full) keeps the picture and
  VCAP as they are and swaps only the io-media graph and the server.
  **When io-media reports an error** (perhaps the HMI took the video for
  its media or the camera), the stream ends without touching
  `/dev/videoctrl`, and the phone is told `REFUSED` (pictures for the rest
  of the connection). If no new stream follows a restart within 5 s, the
  CPU-drawn screen comes back.
- **The phone side** (`android/`, `CaptureService.kt`, `HwEncoder.kt`):
  MediaCodec, Baseline 3.0, CBR at the *H.264 rate* drop-down (2.5, 4
  (default), 6 or 8 Mbit/s). With *Sharp H.264* (default on) the screen
  is captured at k× and box-filtered k:1 on the GPU into the encoder
  (`GlScaler.kt`), as the pictures are; off = the direct capture path,
  which blurs text (docs/phone-mirroring.md §7.13). An IDR only at the start and
  when the MMI asks (`I_FRAME_INTERVAL` 10 s); frames are never dropped
  on their own (the ones after would be garbage): a frame bigger than the
  config's `frame_bytes` (64 KB at first) raises it (a new config, the MMI
  restarts its stream, ~1-2 s black; the size stays for the rest of the
  streaming session, also when the encoder restarts), and a queue of over 25 unacked
  frames is thrown away up to the next IDR. If the encoder puts other
  SPS/PPS before its IDRs than in its codec config, those become the
  config. The phone app's status line shows rate, frame sizes and drops.
- **Desk test** (`make aoa-host`): the PC plays the MMI, checks every frame
  as the MMI would, shows the stream in an 800x480 window laid out like the
  MMI's screen and saves it to `build/aoa-host/stream.h264` (`ffplay` it). The
  window's bottom strip (key D) and the timestamped log tell a freeze (picture
  stays, strip red, "no frames for") from a stop (status panel, "stream ended:
  VIDEO_STOP"). Acks go out at once, so this shows when the phone stops sending,
  not the MMI's timing.
- **Latency:** the phone app's *Latency clock* switch shows a big
  seconds.milliseconds clock. Stream with it on screen and take one photo
  of the phone and the MMI together: the difference is the delay.
- **The screen:** while the stream runs, the main loop closes the
  CPU-drawn screen (the video displayable replaces it) and opens it again
  afterwards.
- **Log:** `h264:` lines: the config, the stream's start and end with its
  totals (`ahead of the clock`: frames the cap dropped, how often, the
  furthest ahead), refused frames (the first 5), `over 500 ms ahead of the
  frame clock` each time the cap starts, and every 10 s frames in, skips,
  io-media's reads and the lag.
- **Tests:** on the build host, `make test` (header, padding, push / fill
  / read, skips decoded by ffmpeg) and `make apk-test` (the phone's
  protocol and H.264 helpers); the same MMI code with a clip instead of
  the phone runs in hello9 (`src/hello9/README.md`). This is a C++ app
  (`docs/cpp-apps.md`).

## Retired frame types: `0x02`, `0x03`, `0x08`

The phone app could once send typed text: `TEXT` (`0x02`, `seq u16` +
UTF-8), answered by `ACK` (`0x03`). It was removed on both sides
(`Proto.kt`, `proto.h`, `tools/aoa_host.py`, the MMI's message list):
the link is for the phone's screen. The numbers are **retired, not
free**: an old phone app that still sends `TEXT` gets `rx unknown frame
type 0x02` in the log and nothing else. Don't reuse them.

`VIDEO_ACK` (`0x08`: `id u16`, `status u8`, `decode_ms u16`) acknowledged one
picture per message. `VIDEO_ACKS` (`0x0D`) replaced it, without the decode time, which
only the phone's status line showed (the MMI's own screen and log still have it).
The APK and the SD card must be from the same build: an older phone app waits 3 s
per window for acks that it does not understand, so it crawls.

## Results on the car

Phone: Redmi Note 8 Pro.

**USB / AOA.** The path works end to end: AOA switch (`GET_PROTOCOL`
version 2, `START`), accessory `18d1:2d00` with endpoints IN `0x81` / OUT
`0x01`, `HELLO` / `HELLO_REPLY`.

- On the first connection the phone needs two AOA `START`s before it
  comes back as an accessory.
- An accessory session that nobody opens on the phone drops after ~18 s;
  the app reconnects by itself.
- In some boots EP0 requests to the same phone failed with
  `DEV_NOANSWER` (docs/usb-stack.md).

**JPEG pictures.** Work, turning the phone included (landscape
800×368 to portrait 220×480 mid-stream, no errors), 0 bad, 0 dropped;
*Details* on/off works.

| | full VFB copy | changed-span copy (`copy=changed`) |
|---|---|---|
| copy into the VFB | ~160 ms (1.5 MB) | 18–19 ms (~30 KB) |
| present (render + copy + flip) | ~200 ms whatever the size | render 23 + copy 18–19 + flip 3 ms |
| rate | ~2 fps | 2–4 fps per 10 s window (avg ~3), set by the phone |

- Decode: landscape 800×368 JPEG 55–65 ms; portrait 220×480
  JPEG ~22 ms. A landscape picture costs the MMI ~100 ms in total, so it
  could do ~10 fps; the phone (*Max fps*, a static screen, its capture and
  compress) sets the rate. Gaps between pictures 92–668 ms.
- Latency (`VIDEO_START` in to on screen): 80–160 ms average, max 285 ms.
- Main loop 18–56 % busy, process CPU ~27 %. The unit's CPU goes from
  ~25 % to 40–60 % while streaming.
- **Blurred text, cause:** the MMI copies the picture 1:1 (ARGB, no
  scaling), so blur comes from the phone. Capturing at the picture's own
  size makes Android scale the 2340×1080 screen down 2.9:1 (landscape) /
  4.9:1 (portrait) in one bilinear step, which skips most of a thin
  stroke's pixels; JPEG q60 added ringing. Hence the k× capture + k×k box
  average and JPEG q90.
- The stream stops when the phone locks: Android revokes the screen
  capture and the phone sends `VIDEO_STOP`. *Keep screen on while
  streaming* stops the screen timeout from locking the phone (the screen
  may dim); the power button still locks it.
- **Phone log:** the app keeps its own log, up to 2 MiB:
  `adb pull /sdcard/Android/data/mmi3g.audistreaming/files/audi_streaming.log`
  (older lines in `.log.1`). It has screen and lock events, focus changes,
  how each projection ended, why Android ended earlier processes, and a
  status line every 5 s. A status line with `encoded` growing but no
  `display frames` means a frozen picture; gaps between lines mean the CPU
  slept.

**H.264.** Works well on the MMI: 720×432 Baseline, text sharp, landscape
only (a portrait phone shows a "turn the phone to landscape" card), 22–25 fps on a moving screen (a still screen sends fewer
real frames, and skip frames fill up to 25). Lag is ~500 ms, ~1.1 s on the
first stream of a session. MMI CPU ~30 % (receiver 6–9 %, up to 15 % with big
frames, against ~45 % for JPEG). Every rotation or frame-size growth restarts
the graph, usually several times in a row, with an occasional harmless
`player: ERROR`: docs/phone-mirroring.md §7.13.

# audi_stream_receiver

Android Auto on the MMI over USB. The Android app (`android/`, "Audi
streaming") is the Android Auto head unit for Android Auto on the same phone
([docs/android-auto.md §4.3](../../docs/android-auto.md)) and sends its
picture over Android Open Accessory (AOA) as H.264, decoded in hardware by
the DM642.

```
phone: android/ app  --USB (AOA bulk)-->  MMI: audi_stream_receiver  -->  screen + log
```

- **Run:** `audi_stream_receiver <sd_path>` — runs until the SD card is
  pulled or SIGTERM/INT/HUP, so it is always the last app of a run.
- **Output** (`var/`): `audi_stream_receiver.log` (previous run kept as `.prev.log`),
  `usb_slog_<time>.txt` at a failed handshake / removal / io-media error (up to 6),
  `usb_slog_periodic_<time>.txt` every 60 s while streaming. `<time>` is time since boot.
- **Build:**

  ```sh
  make sd            # every app on the card; start it from the runner's menu
  make test          # protocol, frame reassembly, live stream, camera tracker
  make apk           # android/out/audi_streaming.apk; make apk-test, make apk-install
  make aoa-host      # desk test: the PC plays the MMI (stop host adb first)
  ```

  The APK and the SD card must come from the same build.

## Using it

1. Install the APK, insert the SD card: the MMI says *Waiting for phone*.
2. Plug the phone in and accept "open Audi streaming": the bar turns green, *Connected*.
3. The app starts its Android Auto mode. Once, on the phone: Android Auto's
   developer settings → *Start head unit server*. The MMI shows Android Auto.
4. Back/Home, unplug/replug keep the stream going. Stop it in the app; pull
   the SD card to end the MMI side.

## How it works

- **USB:** io-usb with a wildcard filter, AOA handshake with any new device;
  the phone's `accessory_filter.xml` matches `mmi3g` / `audi_stream_receiver`.
  Non-Android devices, unplugging and transfer errors never end the app
  ([docs/usb-stack.md](../../docs/usb-stack.md)).
- **Protocol** (`proto.h` = `android/.../Proto.kt`): `type u8 | len u16 LE | payload`,
  max 2048 B. `HELLO`, `VIDEO_CAPS` (boxes, formats, limits), `VIDEO_START` +
  `VIDEO_DATA`, batched `VIDEO_ACKS`, `VIDEO_STOP`, `H264_CONFIG`, `H264_STATUS`
  (`PLAYING`/`SYNC`/`REFUSED`/`CONFIG`), `STREAM_SETTINGS` (phone settings, logged only).
  **Retired, never reuse:** `0x02` TEXT, `0x03` ACK, `0x08` VIDEO_ACK, `0x0A` status bar,
  `0x0F`–`0x12` AUDIO_CAPS / CONFIG / DATA / STOP (the screen mirroring's phone audio),
  format 1 (JPEG) and 2 (PNG).
- **H.264** (`hwvideo.cpp`, `src/common/video/hw_stream.h`): 720×432 (or
  720×464 in the 1:1 mode) Baseline from the phone's MediaCodec → CS-MP4 stream served by our own resource manager
  at `/dev/audi_stream/live.mp4` → io-media → VCAP 0 input 1. 25 fps stream
  clock with skip-frame gap fill; a clock cap (> 500 ms ahead) and an io-media
  catch-up (> 12 frames behind) drop frames and resume from an IDR. Frames
  are capped at 128 KB. A stream declares at most 2 GiB (io-media's read
  offset is signed 32-bit) and restarts when that runs out: ~10.6 min with
  128 KB frames. Up to 3 io-media errors per 60 s restart the stream,
  then `REFUSED`: the phone pauses and tries again later. Details:
  [docs/phone-mirroring.md §7.10–7.13](../../docs/phone-mirroring.md).
- **Rear camera:** [common/camera](../common/camera/README.md) follows the
  camera (the slog and its BAP session on ch4); while it is up the app writes nothing to VP1 and shows
  nothing, and retakes the screen 1 s after it ends
  ([docs/rear-camera.md §9](../../docs/rear-camera.md)).
- **Screen:** while no stream plays, a CPU-drawn status screen (`ui.cpp`):
  link state and what to do next. The stream's video replaces it.
- **Logging:** per-frame paths only count in RAM; `h264:` summaries every
  10 s.

## Log lines

| Line | What |
|---|---|
| `rx HELLO ... build <v>` | the phone app's version: check it first |
| `rx STREAM_SETTINGS` | the phone's settings (video size, fps, bit rate, frame size, …) |
| `cpu:` (10 s) | app and unit CPU, per thread, free RAM, heap; ` - HIGH` at ≥ 50 % own load |
| `h264: config:` | the stream's frame cap and size |
| `h264: 10.0 s:` | frames in, skips, io-media reads/waits/lead, `lag min/median/max`, reader behind, data rate |
| `h264: stream ended` | totals: refused, bad, ahead of the clock, catch-up, evicted |
| `h264: at the error:` | on an io-media ERROR: last 32 reads, last 16 events, slog snapshot |
| `h264: tx H264_STATUS REFUSED (reason)` | why H.264 was refused |
| `camera:` / `videoctrl` | rear camera on/off, VCAP changes |
| `car: night: day -> NIGHT (DA01.0C0D byte 0 = N)` | the HMI's night state changed (`carlog.h`, `common/camera/carsig.h`); unknown until the first change after the start |
| `car: <function> first / [a] -> [b]` | every ch4 function: first value and changes, the changes held back counted. Light functions `0x0C0C` / `0x0C0D`: every change. Car-service blocks `DA01` / `D901` / `FB01` (clamp/energy, driving state, fuel, oil, offroad, …): at most one line per function per 1 s. Cluster, unknown blocks and BAP functions (`BAP 0B.1A`): per 5 s; the BAP clock `11.11` and counter `0F.2A` are left out |
| `car: 60.0 s:` | night state, changes per function, the ch4 reader's state and gaps (`stopped: gaps` = no camera BAP and no car lines from then on) |

`var/audi_stream_receiver_ch4.txt` has every ch4 telegram, decoded and named, BAP streams one line
per function (the previous run's is `*.prev.txt`).

`lag` is MMI-side only (io-media's queue); the phone's decoder, GL copy, encoder and USB are not in it.
It comes from io-media's TIME position while that clock matches the stream. After a catch-up
(its clock runs on through the dropped stretch) it comes from io-media's read position minus
the frames io-media holds (learnt before), counted as `from the read position`.

## Results

![Android Auto mode on the MMI while driving](img/android-auto.jpg)

**Works**
- AOA end to end (Redmi Note 8 Pro); reconnects on its own.
- Android Auto at 25 fps in both picture modes: 720×432 (scaled) and
  720×464 (1:1). MMI lag median ~320 ms at 720×432, ~370–400 ms at 720×464;
  unit CPU ~45–50 %.
- Rear camera takes the screen and the stream comes back after it.
- io-media catch-up: when io-media reads over 12 frames behind, the frames
  in between are dropped, an IDR asked for, and the stream plays on. A
  frame over 64 KB raises the frame size to 128 KB with one restart.
- Restart after an io-media error: the stream comes back by itself (but see
  below: its picture stayed hidden).

**Doesn't work / known**
- After ~190 s io-media reads slower than 25 fps at ~6 Mbit/s and falls
  behind: a catch-up every ~30 s (lag swings ~100–570 ms), at either
  picture size and at any phone frame rate (gap fill keeps the stream at 25).
- Without the 2 GiB cap the stream went black after ~10.6 min (io-media
  EIO at the read at 2 GiB).
- Frame-size growth restarts the graph.
- 256 KB frames made io-media fall behind and fail ~45 s in: hence the 128 KB cap.

**Built, not yet confirmed on the car**
- The picture after an io-media error restart: the restarted stream played,
  but its picture went on while the app's own screen was still closing, and
  the close showed the HMI's displayables over it (black until the user
  restarted). The picture now waits until the app's screen is closed
  (`hwvideo::screen_released`).
- The 2 GiB cap's planned restart (a short black gap every ~10.6 min with
  128 KB frames).

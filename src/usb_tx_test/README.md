# usb_tx_test

The phone's screen on the MMI, over USB. An Android phone app
(`android/`, the usb_tx_test APK) captures its screen and sends it as JPEG
pictures, about two a second, over Android Open Accessory (AOA). This app
receives them, decodes them on the CPU and shows them full screen.

```
phone: android/ app  --USB (AOA bulk)-->  MMI: usb_tx_test  -->  screen + usb_tx_test.log
```

- **Run:** `usb_tx_test <sd_path>`. It runs until the SD card is pulled or
  it gets SIGTERM/INT/HUP, so it is always the last app of a run.
- **Output:** `usb_tx_test.log` on the card (every USBDI result, state
  change, frame and picture). A failed handshake or a removal also saves
  the system log to `var/usb_slog_<n>.txt`. The previous run's files are
  kept as `usb_tx_test.prev.log` and `var/usb_slog_prev_<n>.txt`.
- **Build:** `make` (MMI side), `make apk` (phone app), `make test` and
  `make apk-test` (protocol, pictures, JPEG), `make aoa-host` (desk test:
  the PC plays the MMI).

How the USB side works, what the unit's USB stack does and the full
protocol are in the main [README](../../README.md#usb_tx_test-make-sd-sd_appsusb_tx_test)
and [docs/usb-stack.md](../../docs/usb-stack.md). The frames are in
`proto.h` (= `android/.../Proto.kt`).

## Using it

1. Install the APK on the phone. Insert the SD card. The MMI says
   *Waiting for phone*.
2. Plug the phone into the car's USB port. The status bar goes amber
   (*switching*), then blue. The phone asks to open usb_tx_test for this
   accessory: accept, and the bar turns green, *Connected*.
3. On the phone, tap **Stream screen** and allow the screen capture. The
   phone's screen appears on the MMI: landscape on the whole screen,
   portrait on the left half with the status panel beside it. The
   phone's *Details on the MMI* switch adds a slim status bar over a
   landscape picture.
4. Use the phone as usual. Back, Home, other apps, turning the phone,
   unplugging and replugging all keep the stream going. Stop it in the
   app or from its notification. Pull the SD card to end the MMI side.

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
lines, `usb_tx_perf.csv`).

**Next run:** stream something busy and something mostly static, each for
a while, in both orientations. Then compare `perf:` lines: `copy` and
`vfb KB` should drop well below 160 ms / 1500 KB on static screens. Check
`wait` (time a picture sat before decode; should be > 0 now that two are
in flight) and `main busy` (near 100 % = the MMI is the limit). For an
A/B, put `copy=full` in the arguments field of `sd/apps.conf`.

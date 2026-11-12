# android_auto

An Android Auto head unit, proof of connectivity: a stock Android phone
with the Android Auto app, plugged into the car's USB port, connects to
the MMI and streams its projected screen (works on the car). Nothing is shown yet: the
phone's H.264 video is written to the card for a look on a PC, and the
MMI screen is a status page. Why there is no picture, and what the full
receiver needs: [docs/android-auto.md](../../docs/android-auto.md).

```
phone (Android Auto)  --USB, AOA bulk-->  aoa_host  -->  session  -->  android_auto.h264 + log
```

- **Run:** `android_auto <sd_path> [audio=0|1] [version=M.m] [save=MB] [tls=N]`.
  Runs until the SD card is pulled or it gets SIGTERM/INT/HUP.
  - `audio=0`: advertise no audio channels (default: media 48 kHz stereo,
    guidance and system 16 kHz mono; the PCM is counted and dropped, so the
    phone's sound is silent while connected)
  - `version=`: protocol version asked for (default 1.1)
  - `save=MB`: cap of `android_auto.h264` (default 64; 0 = don't save)
  - `tls=N`: mbedTLS trace level in the log (default 1, errors only; 2 shows
    each handshake step)
- **Output** (in `var/` on the card): `android_auto.log` (USB, every protocol step, a `stats:` line
  every 10 s, a `cpu:` line every 10 s); `android_auto.h264` (Annex-B,
  `ffplay android_auto.h264`); USB slog snapshots in `var/`. The previous
  run's files are kept as `*.prev.*`.
- **Build:**

  ```sh
  make sd SD_APPS=android_auto   # the app + aa/headunit.crt, .key on the card
  make test                      # test_android_auto: the protocol against a fake phone
  make aa-host                   # desk test: this PC is the head unit for the phone
  ```

  The first build downloads mbedTLS and the head unit certificate into
  `third_party/` (below).

## How it works

**USB** ([common/aoa_host.cpp](../common/aoa_host.cpp)): the AOA host of
audi_stream_receiver (every io-usb lesson in
[docs/usb-stack.md](../../docs/usb-stack.md)) as a byte pipe. It sends the
strings Android Auto expects from a head unit (manufacturer `Android`,
model `Android Auto`); the phone then starts Android Auto itself, no app to
install. A bulk OUT block that is a multiple of the packet size goes out as
two transfers ([common/usb_chunk.h](../common/usb_chunk.h)), so a phone
read never waits for a zero-length packet.

**Frames** ([frame.h](frame.h)): `channel | flags | size | [total] |
payload`. A message longer than 16 KB is split into FIRST / MIDDLE / LAST
frames, each encrypted on its own; channels interleave.

**Session** ([session.cpp](session.cpp)), in order:

| Step | Head unit | Phone |
|---|---|---|
| 1 | VERSION_REQUEST 1.1 | VERSION_RESPONSE (its version, status 0) |
| 2 | TLS 1.2 ClientHello in ENCAPSULATED_SSL, ... | ServerHello, ..., asks for the client certificate |
| 3 | AUTH_COMPLETE (status 0); encrypted from here | SERVICE_DISCOVERY_REQUEST (its name) |
| 4 | SERVICE_DISCOVERY_RESPONSE: the services below | CHANNEL_OPEN_REQUEST per service -> OK |
| 5 | SETUP -> CONFIG ready, 1 unacked; VIDEO_FOCUS projected | START (session id), codec config, frames |
| 6 | an ACK per media message; PING every second | PINGs, focus requests, sensor requests |

Services advertised (channel: what):

| Ch | Service | Content |
|---|---|---|
| 1 | input | touch screen 800x480; keys HOME, BACK, D-pad, SEARCH, media, rotary |
| 2 | sensors | driving status (always unrestricted = parked), night mode (always day) |
| 3 | video sink | H.264 Baseline, 800x480, 30 fps, 140 dpi |
| 4, 5, 6 | audio sinks | media 48 kHz stereo, guidance and system 16 kHz mono (with `audio=1`) |

Audio, navigation and video focus requests are always granted. Message ids
and field numbers follow aasdk's `aap_protobuf/*.proto` (cited next to each
message in session.cpp); the protobufs are written and read by hand
([common/pb.h](../common/pb.h)), no generator.

**TLS** ([tls.cpp](tls.cpp), [common/mbedtls/mbedtls_config.h](../common/mbedtls/mbedtls_config.h)):
mbedTLS 3.6 as a TLS 1.2 client in memory. The head unit authenticates with
aasdk's head unit certificate (issued by "Google Automotive Link" to JVC
Kenwood, valid to 2045); the phone's certificate is not checked. Two things
the unit needed:

- **No /dev/random.** Random bytes come from /dev/urandom (pseudo) mixed
  with the jitter of the CPU cycle counter around kernel calls, and seed
  CTR-DRBG directly: mbedTLS's entropy pool (`mbedtls_entropy_func`) fails
  on the unit, so it is only probed and logged (`tls: entropy pool:`). On
  SH, `ClockCycles()` keeps the count in its upper 32 bits.
- **The certificate's dates.** They are `UTCTime` with a `-0700` offset,
  which mbedTLS refuses; [common/mbedtls/utctime-offset.patch](../common/mbedtls/utctime-offset.patch)
  lets the parser skip the offset (the dates are never checked).

**Threads:** io-usb's callback thread queues bulk IN bytes; the session
thread runs the protocol (TLS, acks, the video file); the USB manager thread
handles devices; main draws the status page once a second and on every
change, and gives the screen to the rear camera like audi_stream_receiver.

## Third-party pieces

Fetched by `make` into `third_party/` (gitignored), checked against pinned
SHA-256s:

| What | From | Licence |
|---|---|---|
| mbedTLS 3.6.7 | Mbed-TLS release tarball, patched (above) | Apache-2.0 |
| `headunit.crt`, `headunit.key` | opencardev/aasdk `cert/`, pinned commit | public, see docs/android-auto.md §7 |

On the card the certificate lives in `aa/`; the app refuses to start the
USB side without it.

## Desk test

`make aa-host` runs [host/main.cpp](host/main.cpp) in the `mmi-aa-host`
image: the same session code over libusb, so a phone can be tried at the
PC before the car. Stop adb first. It switches the phone to Android Auto,
runs one session, writes `build/aa-host/android_auto.h264`, `media.pcm`,
`guidance.pcm`, `system.pcm` (s16le) and `android_auto.log`, and with
`--play` (the default `AA_HOST_ARGS`) shows the video in an ffplay window.
Ctrl-C ends it with a bye-bye.

## State

Built; the protocol passes the host test against a fake phone (mbedTLS
server, fragmented video, focus, sensors, key binding, pings, bye-bye).
`make test` runs that session a second time against Google's protobuf
([tests/check_aa_proto.py](../../tests/check_aa_proto.py)): the phone's
messages are encoded by protobuf from aasdk's `.proto` files, and every
message the head unit sends must decode as its type with all required
fields, no unknown field or enum value, and the same bytes when re-encoded.

**On the car it connects** (Redmi Note 8 Pro, MIUI): every status-page step
green, the phone shows "Android Auto is active". The phone accepts aasdk's
certificate and protocol 1.1 (it answers 1.7); TLS 1.2
(ECDHE-RSA-AES128-GCM) is up in ~370 ms; all 7 channels open; the video is
800x480 Baseline L3.1 at ~30 fps, ~0.5-1 Mbit/s, and plays on a PC. The
unit has no entropy source mbedTLS accepts (`entropy pool: -0x003c`), so the
DRBG is seeded by the own jitter source. CPU: the app 15-26 %, the unit
45-50 % while saving the video.

- The first ~20 s are slow: 4-9 fps and ping round trips of 2-4.5 s, then
  30 fps and 15 ms. The MMI's out queue stays empty, so the wait is on the
  phone side (projection and Maps starting) [inferred, no logcat].
- Maps on the car screen asks to change the phone's location settings and
  searches for GPS: the head unit offers no location sensor, so the phone's
  own GPS has to be on and allowed.
- No audio arrived (nothing was played).

Open points:

- showing the video (800 wide is refused by the DM642, docs/android-auto.md
  §4.1), touch / knob input, audio out

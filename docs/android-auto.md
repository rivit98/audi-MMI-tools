# Android Auto on the MMI 3G+ (P1001, rev 51)

Expands option 5 of [phone-mirroring.md](phone-mirroring.md).

**Evidence labels.** *[repo]* = measured or read on this unit and recorded in
this repo. *[recalled]* = protocol knowledge from the open-source projects
listed in section 6, **not checked against their source**.
*[search]* = read from the repos' GitHub pages. *[source]* = read in a
project's source (section 6.1). *[estimate]* = guess, no measurement.

## 1. Verdict

Feasible in principle. The protocol side is ordinary work. The risk sits in
two places that the existing `audi_stream_receiver` work cannot answer:

1. **Live H.264 video at 800x480.** The DM642 hardware decoder refuses
   800-wide input [repo]; live H.264 at up to 720 wide works only through a
   CS-MP4 stream ([native-apps.md](native-apps.md) §10), the path hardware
   mirroring uses. An 800x480 stream with a 80 px margin, cropped to 720
   in the SPS, is the untested way through it (section 4.1). Software
   decode on the SH4 is unmeasured.
2. **Live PCM audio out.** The only audible path found is file-based [repo].

Do the two de-risking experiments (section 5, steps 1-3) before writing any
protocol code. No ready-made **C** implementation exists (section 6); the
usable assets are the protocol definitions and C++ reference code.

**Implementation.** [src/android_auto](../src/android_auto/README.md) is
the connectivity part of section 5 step 4: AOA, frames, TLS, service
discovery, channels, media acks, pings, with a desk build that runs the
same code on the PC (`make aa-host`, which also serves as step 1's spike).
On the car the whole session works [repo]: the current Android Auto app
accepts aasdk's certificate and protocol 1.1, TLS takes ~370 ms on the SH4,
and 800x480 video arrives at ~30 fps and ~0.5-1 Mbit/s. The connectivity
risk is gone; video and audio (above) remain. The video is saved, not shown. Sections 2 and 3 below are checked against
aasdk's source (opencardev/aasdk) where android_auto relies on them.

## 2. How wired Android Auto works [recalled]

1. **AOA.** The phone is the USB device, the head unit (HU) is the host. The HU
   runs the AOA handshake (GET_PROTOCOL, SEND_STRING x6, START) with model
   "Android Auto". The phone re-enumerates as 18d1:2d00/2d01 with one bulk IN
   and one bulk OUT endpoint.
2. **Framing.** Everything after that is channel-multiplexed frames on the two
   bulk pipes: channel id, flags (first / middle / last fragment, encrypted,
   control), length (plus total length on a first fragment).
3. **Control channel (0).** Version request/response, then a TLS 1.2 handshake
   tunnelled through control frames (HU = TLS client, with a HU certificate),
   then auth-complete. Afterwards most channels are encrypted, video included.
4. **Service discovery.** The phone asks, the HU answers with a protobuf list:

| Service | Content |
|---|---|
| Video sink | H.264 Annex-B. HU advertises codec (Baseline is available), resolution, fps. Phone paces itself on HU acks. |
| Media audio | 48 kHz stereo 16-bit PCM |
| Guidance / system audio | 16 kHz mono PCM |
| Mic source | Voice assistant. Optional. |
| Input source | Touchscreen, D-pad keycodes, rotary, touchpad. |
| Sensors | Driving status, night mode (minimum); GPS optional. |
| Bluetooth, nav status, media metadata | Optional |

5. **Runtime.** The phone renders and encodes the UI. The HU decodes, plays
   audio, sends input, answers pings and audio/video focus requests, acks
   every video frame.

## 3. What the MMI needs

| Piece | State | Notes |
|---|---|---|
| AOA transport | **Built** | `common/aoa_host.cpp`: audi_stream_receiver's AOA host as a byte pipe, with the Android Auto strings (manufacturer `Android`, model `Android Auto`). USB speed of the host port not checked. |
| Frame layer, control channel | **Built** | C++ (`frame.h`, `session.cpp`), host-tested against a fake phone. |
| TLS 1.2 client | **Built** | mbedTLS 3.6 compiled for the unit (no TLS library on it [repo]). The unit has no `/dev/random`: entropy from cycle-counter jitter. Handshake time on the SH-4 not measured. |
| HU certificate + key | Fetched at build time | aasdk's `cert/headunit.crt` (issued by "Google Automotive Link" to JVC Kenwood, valid to 2045) + key, not in the repo. Its dates carry a `-0700` offset mbedTLS refuses without a one-line patch. The current app accepts it (section 7). |
| Protobuf messages | **Built** | Written and read by hand (`src/common/pb.h`), about 30 messages; no generator. |
| Input | Partly | The MMI has no touchscreen. Advertise rotary + D-pad. Knob detents and presses are decoded from `/dev/ipc/ch8` [repo]; push directions and other buttons are still open (platform-notes §8). |
| Sensors | **Built** | Fixed values: driving status unrestricted (parked), night mode off. |
| Video | **Blocked at 800 wide** | Same pipeline as hardware mirroring, which plays 720 wide; an SPS crop to 720 is the untested way past (section 4.1). |
| Audio | **Unproven** | Section 4.2. |

### 4.1 Video

**How AA video works.** The head unit only decodes. In service discovery
it offers video configurations (800x480, 1280x720 or 1920x1080; 30 or
60 fps; margins, density). The phone renders the car UI (maps, media,
launcher) into a virtual display of that size, encodes it as H.264
Baseline, and sends the codec config (SPS/PPS) and then the frames on the
video channel. The head unit acks each frame (the acks pace the phone, up
to `max_unacked` in flight), shows the picture full screen, and sends
touch, knob and button events back on the input channel.

**The same pipeline as hardware mirroring.** This is option 2a of
[phone-mirroring.md](phone-mirroring.md): there the phone app encodes
H.264 and audi_stream_receiver feeds the frames through LIVE (`hw_stream`,
`csmp4`, `stream_server`, `player`, `vout`, the rear-camera handover) to
the DM642, which plays a live CS-MP4 stream on screen [repo]. LIVE does not
care where the frames come from, so android_auto can hand AA's frames to
the same `HwStream`. What differs is who controls the encoder:

| | Mirroring (option 2a) | Android Auto |
|---|---|---|
| Encoder settings | ours: 720x432, 25 fps, Baseline | the phone's, from the offered list only: **800x480 minimum** |
| Width | 720: plays [repo] | 800: **io-media refuses width > 720 or height > 576** (`FUN_080d1760`, phone-mirroring §7.8), and the DSP's `NvMixer_Video.c` has `assert(width < 720)` [repo] |
| Level | ≤ 3 | 800x480 at 30 fps is 45 000 MB/s, above level 3's 40 500, which io-media rejects; the phone may signal 3.1 (patching `level_idc` in the SPS: unverified) |
| Frame rate | 25 | 30 or 60; 30 is allowed at 480 lines |
| Pacing | batched `VIDEO_ACKS` | the protocol's media acks, held to 25 fps (ack pacing) |

So the 720-wide limit is the one blocker, the same one an 800x480 clip hit
in the mirroring tests.

**Idea to pass it: margin + SPS crop [estimate, untested].**

1. Offer 800x480 with `width_margin = 80`: the phone keeps the car UI in
   the left 720 columns and fills the rest with black, still encoding
   800x480.
2. Rewrite the SPS with `frame_cropping` of 80 px on the right, so the
   stream says 720x480; the CS-MP4 header (`csmp4`) declares 720x480 and
   passes io-media's check. The SPS is a few bytes, rewritten once per
   codec config; `common/h264.h` already parses cropping.

The rewrite is built: `h264::crop_sps` sets the cropping and `level_idc`
and keeps the rest of the SPS bit for bit; ffmpeg decodes a cropped stream
to exactly the original's top-left [repo, `make test`]. At 25 fps the
800-wide coded picture is 37 500 macroblocks/s, within Level 3, so level
3.0 in the rewritten SPS is true.

Open: whether the DM642 decodes a 50-macroblock-wide coded picture that is
cropped to 720 for display, or trips its width assertion or overflows a
buffer sized for 720. [hello10](../src/hello10/README.md) answers it
without a phone: an ffmpeg 800x480 Baseline clip with the cropped SPS,
streamed through `HwStream` on VCAP 0 input 1, also cropped to 720x416
(the bottom bar cut). If it plays, AA video on screen is mostly wiring. If
not, the options are below.

**Frame rate: ack pacing [estimate].** The phone encodes at 30 fps, the
stream plays at 25. android_auto holds each video ack until a frame period
after the last (`fps=`, default 25; its README): with one frame unacked,
the phone can't send faster. Whether it then drops frames (the age of
arrivals stays flat) or queues them (it grows) is unmeasured.

- **DSP driven directly** (option 2b): bypasses io-media's check, but the
  DSP assertion stays; months of work.
- **Software decode, Baseline.** Cheapest profile (no CABAC, no B-frames). Cost
  chain per frame: decode + YUV to RGB on the CPU + texture upload (31 ms for
  800x480 RGBA measured [repo]). My guess is 8-15 fps on mostly static UI
  [estimate]. P-frames cannot be dropped, so the decoder must keep up on
  average or latency grows until the ack window throttles the phone.
- **Not usable:** the GPU's VCAP 1 "Partial Decode" is suspected MPEG-2 only
  [repo].

### 4.2 Audio

`mixer:DSPOUT_TDM1_20_SAD` is audible, replaces the radio, mutes BT and
follows the volume knob (hello5) [repo]. It is used through
`testapp-file` with ~350 ms added per queued file [repo]. Nothing found yet
that takes a live, gapless PCM stream. If none exists, AA runs video-only.
Mic input (voice assistant) is not investigated; leave the mic service out
at first.

## 5. Suggested order

1. **PC spike.** Run an existing AA HU implementation (section 6) on the PC
   against the real phone. Confirms the current Android Auto app accepts it,
   and records the raw H.264, PCM and a protocol trace. Read off actual
   resolution, profile, level, bitrate, fps.
2. **Decoder benchmark on the car.** hello-style test that decodes the
   recorded stream with a C Baseline decoder and logs ms per frame, split
   into decode / colour convert / upload. Settles software video.
3. **Live PCM test.** Find any path for continuous PCM to TDM1_20_SAD (iom
   input types, `/dev/snd`, FPGA streams from hello5).
4. **Build the C stack** reusing `audi_stream_receiver`: frames, nanopb, mbedTLS,
   services, knob input.

Go/no-go after steps 2 and 3. If software decode is under ~15 fps and no
live audio path exists, AA would be a poor experience and option 0/1
mirroring stays the better investment.

## 6. Existing implementations

The only C head unit is the 2015 original of headunit, bound to Android
(JNI). Everything else is C++ (or Kotlin/Rust), and all of it is **GPLv3 or
AGPL-3.0**.

| Project | Role | Language / licence | Use for us |
|---|---|---|---|
| [opencardev/aasdk](https://github.com/opencardev/aasdk) | Maintained HU protocol library: USB/AOAP, TLS, framing, one service per channel. Deps: Boost, libusb, OpenSSL, protobuf. | C++, GPLv3 | **Reference** for the state machine; also the PC spike (step 1). |
| [f1xpl/aasdk](https://github.com/f1xpl/aasdk), [f1xpl/openauto](https://github.com/f1xpl/openauto) | Original (2018, frozen) | C++, GPLv3 | Smaller, easier to read than the forks, but older protocol. |
| [opencardev/openauto](https://github.com/opencardev/openauto) | HU application on aasdk + Qt (Crankshaft). | C++, GPLv3 | PC spike front end; shows service configuration. |
| [mrmees/open-android-auto](https://github.com/mrmees/open-android-auto) | 248 `.proto` files, message and channel docs, session lifecycle, rotary/touchpad input docs, decompiled-firmware notes. Reverse-engineered from AA APK 16.1-17.3. | Protos + docs, GPLv3 | Protocol reference, newer than aasdk's protos. |
| [average-web-dev/aasdk](https://github.com/average-web-dev/aasdk) | OpenHU's aasdk fork | C++, GPLv3 | Alternative fork. |
| [mikereidis/headunit](https://github.com/mikereidis/headunit) | The original Android HU app (2015, frozen): AAP in C (`jni/hu_aap.c`, `hu_ssl.c` on OpenSSL, `hu_usb.c` on libusb), UI in Java | C + Java, AGPL-3.0 | The only C implementation, but old protocol, hand-coded byte arrays, JNI-bound. Reference only (6.1). |
| [andreknieriem/open-headunit](https://github.com/andreknieriem/open-headunit) (headunit-revived) | Maintained fork of headunit: HU on Android, wired + wireless, current AA (17.4+) | Kotlin + protobuf-java, AGPL-3.0 | Not runnable on QNX; **best record of current phone behaviour** (6.1). |
| [openDsh/openauto](https://github.com/openDsh/openauto) | openauto fork on aasdk + Qt (Dash) | C++, GPLv3 | Service and input handling (6.1); old aasdk protos. |
| [tomasz-grobelny/AACS](https://github.com/tomasz-grobelny/AACS), [uglyoldbob/android-auto](https://github.com/uglyoldbob/android-auto), [prototux/AARE](https://github.com/prototux/AARE) | **Phone-side** implementations (the other end of the wire) | C++ / Rust / Kotlin | Useful as a protocol cross-check, and for a desk test that needs no real AA app. |

**Licensing consequence.** Anything derived from aasdk or its `.proto` files
inherits GPLv3. A from-scratch C implementation that only follows the wire
format is a design choice; check the toolkit's own licence before copying
code or generated files into it.

**What android_auto took.** No code: its session, frames and protobuf are
written from the wire format (C++, `src/android_auto`, `src/common/pb.h`),
mbedTLS supplies the crypto. aasdk is used as a reference and, in the host
tests only, through its `.proto` files (`tests/check_aa_proto.py`) and its
head unit certificate.

### 6.1 What the head unit projects teach [source]

Copies of headunit, open-headunit and openDsh/openauto are kept in
`third_party/` (gitignored) for reading. None of their code is reusable on
the MMI (licence, language, dependencies); these facts are:

- **Version status.** Phones answer VERSION_RESPONSE with statuses other
  than 0 on sessions that work; only -1 (STATUS_NO_COMPATIBLE_VERSION)
  refuses, and TLS must be tried on anything else (open-headunit
  `AapVersionNegotiation.kt`). The phone picks the minor version; a
  VersionResponseOptions protobuf may follow the status. open-headunit asks
  for 1.2 and sends the request up to 3 times on USB
  (`HandshakeMessagePolicy.kt`). android_auto refuses only on -1.
- **A second head unit certificate.** open-headunit's `res/raw/cert` +
  `privkey`: "Google-Android-Reference", issued by Google Automotive Link,
  valid 2014-2044, plain `Z` dates (no mbedTLS patch needed). The fallback
  if the phone refuses aasdk's JVC Kenwood one.
- **Flow control.** open-headunit announces `max_unacked` 12 for video and
  30 per audio sink at setup (`UplinkStallMonitor.kt`); android_auto
  announces 1, which ties the frame rate to the ack round trip. A head unit
  whose acks stall loses video and audio together while control traffic
  continues.
- **Pings.** open-headunit only answers the phone's pings and sends none.
- **Video config.** open-headunit also sets `pixel_aspect_ratio_e4` and the
  margins, and offers a second, smaller resolution: a phone that refuses
  the first walks the list (`ServiceDiscoveryResponse.kt`).
- **Rotary input.** A knob detent is an InputReport with
  `relative_event {keycode 65536 (ROTARY_CONTROLLER), delta ±1}`; a button
  is `key_event {keycode, down, metastate 0}`, press and release
  (openauto `InputService.cpp`). The MMI's decoded knob (hello7) maps onto
  this.
- **Wireless without Bluetooth.** Android Auto's developer setting "Start
  headunit server" makes the phone listen on TCP 5277; the HU connects and
  speaks the same AAP stream (headunit `hu_tcp.c`, open-headunit
  "Headunit Server"). The MMI has a WLAN access point (`uap0`), so this is
  a route without the USB cable; android_auto's session takes plain bytes
  and would need only a socket transport. Android Auto 17.4+ blocks the
  older wireless triggers, not this one.
- **Debugging.** `headunit/jni/hu_aad.c` and open-headunit `AapDump.kt`
  decode traffic for logs; open-headunit's `LinkGapMonitor.kt`,
  `UplinkStallMonitor.kt` and `ProjectionWatchdogPolicy.kt` describe
  real-world stalls (phone stops sending, acks stuck, video never asked for
  again).

## 7. Open questions

- **HU certificate.** aasdk's README calls its certificate public and "not
  a secret"; it is a JVC Kenwood head unit certificate issued by Google's
  automotive CA, used by every open-source head unit. Its legal status is
  unclear, so it is downloaded at build time and never committed. The
  current Android Auto app accepts it, on the desk and on the car [repo].
- **Protocol drift.** The app is closed and updated often (open-android-auto
  tracks GAL up to 6.0 [search]). Pin a phone/app version for development.
- **Host port.** USB speed and power (AA drains the battery without
  charging) not checked.
- **Display path.** Software video needs an upload path faster than the VFB
  copy (full copy ~160 ms, changed spans ~18 ms [repo]); GL upload is 31 ms.
- **Driving restrictions.** A fixed "unrestricted" driving status is
  accepted when parked [repo]; whether the phone insists on real
  speed/gear data while moving is unknown.
- **Location.** No location sensor is offered, so Maps uses the phone's GPS
  and asks for its location settings when that is off.

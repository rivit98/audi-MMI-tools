# Android Auto on the MMI 3G+ (P1001, rev 51)

Expands [phone-mirroring.md](phone-mirroring.md) §4 (Android Auto receiver).

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
   mirroring uses. Cropping the SPS of an 800x480 stream to 720 stays
   black (section 4.1). The way around it is on the phone: the phone app's
   Android Auto mode decodes Android Auto's video and sends the MMI a
   720x432 stream (section 4.3). Software decode on the SH4 is unmeasured
   (hello10's `sw` test measures it).
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
risk is gone; video and audio (above) remain. The video is counted and dropped, not shown. Sections 2 and 3 below are checked against
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
| Video | **Blocked at 800 wide** on the MMI | An SPS crop to 720 stays black (section 4.1). The phone app's Android Auto mode re-encodes on the phone instead (section 4.3). |
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

**The same pipeline as hardware mirroring.** This is the H.264 path of
[phone-mirroring.md](phone-mirroring.md): there the phone app encodes
H.264 and audi_stream_receiver feeds the frames through LIVE (`hw_stream`,
`csmp4`, `stream_server`, `player`, `vout`, the rear-camera handover) to
the DM642, which plays a live CS-MP4 stream on screen [repo]. LIVE does not
care where the frames come from, so android_auto can hand AA's frames to
the same `HwStream`. What differs is who controls the encoder:

| | Mirroring (H.264) | Android Auto |
|---|---|---|
| Encoder settings | ours: 720x432, 25 fps, Baseline | the phone's, from the offered list only: **800x480 minimum** |
| Width | 720: plays [repo] | 800: **io-media refuses width > 720 or height > 576** (`FUN_080d1760`), and the DSP decoder and its video signal are 720 wide (phone-mirroring §7.14) [repo]; cropped to 720 in the SPS it stays black [repo] |
| Level | ≤ 3 | 800x480 at 30 fps is 45 000 MB/s, above level 3's 40 500; io-media only refuses `level_idc` > 39, so 3.1 passes the host (phone-mirroring §2.1) [repo] |
| Frame rate | 25 | 30 or 60; 30 is allowed at 480 lines |
| Pacing | batched `VIDEO_ACKS` | the protocol's media acks, held to 25 fps (ack pacing) |

So the 720-wide limit is the one blocker, the same one an 800x480 clip hit
in the mirroring tests.

**Margin + SPS crop: does not work [repo].** The idea was to
offer 800x480 with `width_margin = 80` (the phone keeps the car UI in the
left 720 columns) and rewrite the SPS with `frame_cropping` of 80 px on the
right, so the stream says 720x480 and passes io-media's check.
The rewrite set the cropping and `level_idc` 3.0 (800x480 at 25 fps is
37 500 macroblocks/s, within Level 3) and kept the rest of the SPS bit for
bit; ffmpeg decoded the cropped stream to exactly the original's top-left.

On the car, a test app streamed an ffmpeg 800x480
Baseline clip with the cropped SPS through `HwStream` on VCAP 0 input 1:

- io-media accepts it: the graph reports 720x480 (720x416 for the bottom
  crop), TIME events run at 1x for the whole test, 0 errors, reads block
  ~85 ms as with 720-wide streams.
- **The screen stays black** for all three crop tests (720x480, 720x416,
  15 fps with skip frames). The plain 720x336 MP4 control just before plays.
- No assertion, DSP or io-media error in the system log; the unit and the
  following apps run on normally.

So the DM642 shows nothing from a 50-macroblock-wide coded picture,
whatever the SPS says [repo]. io-media configures the DSP with the `avc1`
size (720x480), and the decoder, created for at most 720 wide, meets a
50-macroblock picture; nothing is logged (phone-mirroring §7.14)
[inferred]. Android Auto's smallest video (800x480) can't
reach the screen through io-media. The options left:

- **Phone relay** (section 4.3): the phone decodes Android Auto and sends
  the MMI a 720-wide stream. Built.
- **DSP driven directly** (phone-mirroring §4): bypasses io-media's check, but the
  DSP's decoder and video signal stay 720 wide; months of work. What an
  800-wide picture would take (DSP image, io-media and VCAP patches):
  phone-mirroring §7.14.
- **Software decode, Baseline.** Cheapest profile (no CABAC, no B-frames). Cost
  chain per frame: decode + YUV to RGB on the CPU + texture upload (31 ms for
  800x480 RGBA measured [repo]). My guess is 8-15 fps on mostly static UI
  [estimate]. P-frames cannot be dropped, so the decoder must keep up on
  average or latency grows until the ack window throttles the phone.
- **Not usable:** the GPU's VCAP 1 "Partial Decode" is suspected MPEG-2 only
  [repo].

### 4.3 Phone relay: the phone app's Android Auto mode

The DM642 can't show what Android Auto sends, but it shows what the phone
app sends. So the "Audi streaming" app on the phone also acts as the head
unit for Android Auto on the **same phone** ("self mode", as open-headunit
and Headunit Reloaded do), and sends the MMI the picture through the
mirroring's H.264 path.

```
Android Auto (phone)  --TCP 127.0.0.1:5277-->  AaService (same phone)
   session: src/android_auto through JNI (aa_jni.cpp, NDK, mbedTLS)
   800x480 H.264 --> MediaCodec decoder --> GlScaler: 800x480 -> 720x432 bilinear, Max fps (<= 25)
                                               |--> phone preview (touch input back to AA)
                                               '--> MediaCodec encoder 720x432 --> MmiH264
   --> AOA --> audi_stream_receiver --> DM642 (unchanged)
```

**How it works [source; works on the car].** Android Auto 17.4 and newer
start a projection to a third-party app only through its developer "head
unit server": in Android Auto's settings, developer mode (tap Version),
then menu, Start head unit server; it listens on 127.0.0.1:5277
(open-headunit's `SelfLauncherV17_4.kt`). The app connects there and runs
the same session code as the car app (`src/android_auto`, built with the
NDK). Differences from the car app:

- **The whole frame is scaled to the MMI's picture.** Android Auto ignores
  a `width_margin` of 80 and draws its UI over all 800 columns [repo, desk
  test with the phone app and on the car]: a 1:1 copy of the top-left
  720x432 cut the UI's right edge off. The session asks for 800x480 with no
  margins, and the GPU scales the whole frame bilinear to 720x432: 0.9 both
  ways, so the 5:3 shape stays. Text is slightly softer than 1:1.
- **Video size setting: 16:9 without the rail.** At 800x480 Android Auto
  draws its bar at the bottom [repo, desk]. At 1280x720 and 1920x1080 it
  draws it as a rail on the left instead, 59 of 720 columns of the scaled
  picture at both sizes = ~80 dp at 210 / 315 dpi [repo, desk; dpi raised
  with the height so the layout stays the size of 800x480's]. The app
  leaves the rail out (105 / 158 px) and scales the rest (1175x720 /
  1762x1080) to fill 720x432: ~2% wider than its shape, unnoticeable, and
  the UI is the same size on the MMI as at 800x480. The shrink (0.61 /
  0.41) averages 2x2 bilinear taps over each output pixel's footprint
  (`GlBox.scaledTaps`) so thin text isn't skipped. The phone's preview
  shows the whole frame, rail included: home, apps and mic are tapped
  there until the MMI's knob drives them. A new size ends the session
  (bye-bye) and the app reconnects. Sharpness on the MMI: [unverified].
- **Frame rate and stream settings.** Android Auto sends up to 30 fps; the
  GPU's frame clock draws exactly "Max fps" a second into the encoder (at
  most 25, the MMI's stream rate) and drops the rest. No ack pacing
  (`fps=0`): the acks go at once. "Max fps", "H.264 rate"
  and "Keep screen on" are the mirroring's settings, shown on both pages. A
  new rate or bit rate gets a new encoder that the GPU draws into
  (`GlScaler.retarget`); the decoder keeps running, so Android Auto never
  notices, and only the MMI restarts its stream.
- **Audio.** Only the system-sound sink is advertised (open-headunit's Self
  Mode does the same), so music and navigation stay on the phone and reach
  the car over Bluetooth like any phone sound. System sounds play through
  the phone's speaker path (`AaAudio`).
- **Input.** The phone shows the same 720x432 picture; touches on it go to
  Android Auto as touch events (`Session::touch`), mapped back to the
  800x480 video's pixels.
- **Modes.** The app has two pages, Screen (mirroring) and Android Auto;
  sliding between them switches the mode, and only one streams to the MMI.
  The MMI side (audi_stream_receiver) is unchanged.

**On the car [repo]:** the relay ran for 27 min at a steady 25 fps with the
phone locked and its screen off; MMI-side lag 300-430 ms, MMI CPU ~55-60 %;
the rear camera took the screen and gave it back. With 256 KB frame slots
io-media fell behind and failed with EIO ~45 s after each config; the MMI
now caps frames at 128 KB and catches io-media up (audi_stream_receiver's
README) [built, not yet confirmed on the car].

**Open:**

- Whether the 0.9 scale keeps text readable on the MMI.
- Latency end to end (Android Auto's encoder, the phone's decoder and
  encoder, then the MMI's ~0.4 s).

**Ideas for later:**

- **The MMI's knob and buttons.** Knob detents and presses are decoded from
  `/dev/ipc/ch8` [repo]. A new AOA message (MMI to phone) could carry them,
  and the phone would send Android Auto rotary and D-pad key events (the
  session already advertises the keycodes). The phone then stays in the
  pocket.
- **Night mode** from the car's light state, as the HMI's night map does
  (`DA 01` / `0x0C0D` on ch4, docs/vehicle-data.md §5.2 [repo]; hello11
  checks it), instead of always day.
- **Older Android Auto (17.3 and earlier):** open-headunit's legacy route
  starts Android Auto's `WirelessStartupActivity` with host 127.0.0.1 and
  port 5288 and listens there. Not built: the user's phone needs only the
  17.4 route.
- **Start the head unit server without the menu.** Not possible from a
  third-party app on 17.4+ as far as open-headunit knows; the app opens
  Android Auto's settings to make it two taps.
- **1280x720 or 1920x1080 instead of 800x480**, with dpi raised to match:
  more pixels for the same UI, scaled down by more for sharper text. Android
  Auto ignores the width margin, so these 16:9 frames don't fit 5:3 without
  cropping or black bars. Only if 0.9 at 800x480 looks soft on the car.
- **A plain mirror fallback:** if self mode fails, mirroring Android Auto's
  own phone-screen UI is not possible either (Google removed it); the
  Screen page with Google Maps is the fallback.

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
live audio path exists, AA would be a poor experience and plain
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

# Android Auto on the MMI 3G+ (P1001, rev 51)

Expands option 5 of [phone-mirroring.md](phone-mirroring.md).

**Evidence labels.** *[repo]* = measured or read on this unit and recorded in
this repo. *[recalled]* = protocol knowledge from the open-source projects
listed in section 6, **not checked against their source**.
*[search]* = read from the repos' GitHub pages. *[estimate]* =
guess, no measurement.

## 1. Verdict

Feasible in principle. The protocol side is ordinary work. The risk sits in
two places that the existing `audi_stream_receiver` work cannot answer:

1. **Live H.264 video at 800x480.** The DM642 hardware decoder refuses
   800-wide input [repo]; live H.264 at up to 720 wide works only through a
   CS-MP4 stream ([native-apps.md](native-apps.md) §10). Software decode on the SH4 is
   unmeasured.
2. **Live PCM audio out.** The only audible path found is file-based [repo].

Do the two de-risking experiments (section 5, steps 1-3) before writing any
protocol code. No ready-made **C** implementation exists (section 6); the
usable assets are the protocol definitions and C++ reference code.

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
| AOA transport | **Done** [repo] | `audi_stream_receiver` works. Change the strings and switch to the 2d00/2d01 PIDs. USB speed of the host port not checked. |
| Frame layer, control channel | To build | Plain C. |
| TLS 1.2 client | To build | **No TLS library on the unit** [repo]. Compile mbedTLS or BearSSL. One handshake per connection. Bulk AES at ~1 MB/s is no problem at 792 MHz [estimate]. |
| HU certificate + key | Open question | aasdk ships a `cert/` directory [search]. Source and legal status of that material is unclear; see section 7. |
| Protobuf messages | To build | nanopb (C) generated from the `.proto` files in section 6. Full C++ is possible (hello8: modern static libstdc++ works [repo]), but boost + protobuf C++ is too heavy. The "GCC 3.3 libstdc++" objection in phone-mirroring.md option 5 is outdated. |
| Input | Partly | The MMI has no touchscreen. Advertise rotary + D-pad. Knob detents and presses are decoded from `/dev/ipc/ch8` [repo]; push directions and other buttons are still open (platform-notes §8). |
| Sensors | To build | Send fixed values: parked, day mode. |
| Video | **Blocked / unmeasured** | Section 4.1. |
| Audio | **Unproven** | Section 4.2. |

### 4.1 Video

- **DM642 hardware decoder.** H.264 Baseline 720x480 and 720x336 MP4 files
  reach the screen via displayable 0x101 + VCAP0 input 1. 800-wide,
  fMP4, H.264-in-PS and growing files fail with iom_errno 0x80002 [repo]. The
  DSP source has a `width < 720` assertion [repo]. AA's smallest advertised
  size is 800x480 [recalled], and the stream cannot be cropped or rescaled
  without re-encoding. Hardware decode therefore needs the DSP driven
  directly (option 2b, months) or a surprise from a raw elementary-stream
  input to io-media.
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

No pure-C head-unit implementation turned up [search]. Everything is C++
(or Kotlin/Rust) and almost all of it is **GPLv3**.

| Project | Role | Language / licence | Use for us |
|---|---|---|---|
| [opencardev/aasdk](https://github.com/opencardev/aasdk) | Maintained HU protocol library: USB/AOAP, TLS, framing, one service per channel. Deps: Boost, libusb, OpenSSL, protobuf. | C++, GPLv3 | **Reference** for the state machine; also the PC spike (step 1). |
| [f1xpl/aasdk](https://github.com/f1xpl/aasdk), [f1xpl/openauto](https://github.com/f1xpl/openauto) | Original (2018, frozen) | C++, GPLv3 | Smaller, easier to read than the forks, but older protocol. |
| [opencardev/openauto](https://github.com/opencardev/openauto) | HU application on aasdk + Qt (Crankshaft). | C++, GPLv3 | PC spike front end; shows service configuration. |
| [mrmees/open-android-auto](https://github.com/mrmees/open-android-auto) | 248 `.proto` files, message and channel docs, session lifecycle, rotary/touchpad input docs, decompiled-firmware notes. Reverse-engineered from AA APK 16.1-17.3. | Protos + docs, GPLv3 | **Best concrete asset:** feed to nanopb. Newer than aasdk's protos. |
| [average-web-dev/aasdk](https://github.com/average-web-dev/aasdk) | OpenHU's aasdk fork | C++, GPLv3 | Alternative fork. |
| Headunit-style Android apps (headunit-revived, open-headunit) | HU running on Android | Kotlin, AGPL-3.0 | Not useful on QNX. |
| [tomasz-grobelny/AACS](https://github.com/tomasz-grobelny/AACS), [uglyoldbob/android-auto](https://github.com/uglyoldbob/android-auto), [prototux/AARE](https://github.com/prototux/AARE) | **Phone-side** implementations (the other end of the wire) | C++ / Rust / Kotlin | Useful as a protocol cross-check, and for a desk test that needs no real AA app. |

**Licensing consequence.** Anything derived from aasdk or its `.proto` files
inherits GPLv3. A from-scratch C implementation that only follows the wire
format is a design choice; check the toolkit's own licence before copying
code or generated files into it.

**Realistic "use something concrete" plan:** take the `.proto` set from
open-android-auto, generate nanopb C with a trimmed options file (only the
messages for the services we advertise), and port the aasdk control/TLS/
service flow by hand into C on top of `audi_stream_receiver`. mbedTLS or BearSSL
supply the crypto. The C++ projects run unchanged on the PC for step 1.

## 7. Open questions

- **HU certificate.** Which certificate the phone accepts today, where it
  legitimately comes from, and its legal status. The aasdk `cert/` directory
  is the starting point; whether a given cert is accepted by the current
  Android Auto app is what step 1 tells us. A search summary claimed that
  Google's app only accepts Google-issued certificates; that conflicts with
  these projects working with stock phones and was not verified either way.
- **Protocol drift.** The app is closed and updated often (open-android-auto
  tracks GAL up to 6.0 [search]). Pin a phone/app version for development.
- **Host port.** USB speed and power (AA drains the battery without
  charging) not checked.
- **Display path.** Software video needs an upload path faster than the VFB
  copy (full copy ~160 ms, changed spans ~18 ms [repo]); GL upload is 31 ms.
- **Driving restrictions.** Whether fixed "parked" sensor data is accepted
  or the phone insists on real speed/gear data.

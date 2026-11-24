# Native C/C++ apps for the MMI 3G+

Plain C and C++20 programs for the Audi MMI 3G+ (QNX 6.3.2, Renesas SH7785 /
SH-4A, little endian), cross-compiled with Debian's stock SH4 GCC 16 inside
Docker. No QNX SDK: the toolchain links against stubs generated from the
unit's own libraries. How it works, how to write an app and every platform
rule: [docs/native-apps.md](docs/native-apps.md).

## Build and run

```sh
make sdk                        # once: the mmi-sdk Docker image (toolchain + QNX sysroot)
make sd                         # build every app + lay out build/sd/ (all apps in apps/)
make test                       # host unit tests
tools/sync_sd.sh /media/$USER/CARD   # copy build/sd/ to the card
```

`tools/sync_sd.sh` copies the **contents** of `build/sd/` to the root of
a FAT32 card with rsync: only what changed (`make sd` keeps file times),
files gone from `build/sd/` are deleted, the card's `var/` is left alone.
It refuses a path that isn't the root of a mounted FAT file system. Boot the MMI fully, insert the
card in SD slot 1: the [runner](src/runner/README.md)'s menu comes up, the
main knob chooses an app and its press runs it. Any binary copied into
`apps/` on the card shows up in the menu. Everything a run writes
(logs, screenshots, recordings) comes back in `var/` on the card, so
copying that one folder gets all of it. If the screen freezes, reboot the MMI:
hold **MENU + rotary knob + top-right soft key**. Details:
[docs/native-apps.md §4–5](docs/native-apps.md#4-build-system).

## Apps

Each app lives in `src/<app>/` with a README (what it verifies, what works
and what doesn't). Shared code is in [src/common/](src/common/); the knob and button library
(ch8 events for any app) in [src/common/buttons/](src/common/buttons/README.md),
the rear camera library (is the camera up; the one ch4 reader) in
[src/common/camera/](src/common/camera/README.md).

| App | What it is |
|---|---|
| [hello](src/hello/README.md) | toolchain smoke test: writes `hello.txt` |
| [lmprobe](src/lmprobe/README.md) | layer manager without GL: colour bars straight into the VFB |
| [hello2](src/hello2/README.md) | OpenGL ES bring-up, full EGL/GL diagnostics |
| [hello3](src/hello3/README.md) | GPU canvas, fill-rate benchmark |
| [hello4](src/hello4/README.md) | 3D pipeline probes, scenes, triangle-rate benchmark |
| [hello5](src/hello5/README.md) | sound from an app through io-media; step `p`: the MMI processes' channels; step `d` (HBDSI nav voice channel) disabled, it breaks MMI3GApplication |
| [hello6](src/hello6/README.md) | phone-mirroring checks: benchmarks, DM642 video decode |
| [hello7](src/hello7/README.md) | ch8 input tester: steering wheel and touch pad (every pad mode, set by writing the HMI's command), one step at a time; free mode decodes everything |
| [hello8](src/hello8/README.md) | C++ runtime and standard library checks |
| [hello9](src/hello9/README.md) | live H.264 stream through the DSP |
| [hello10](src/hello10/README.md) | 800×480 video: io-media capture-width probe, a second io-media |
| [hello11](src/hello11/README.md) | read only: rear-camera, light (night map) and light-sensor / dimming signals on `/dev/ipc/ch4`, `/dev/ipc/ch5`, ndr and slog |
| [hello12](src/hello12/README.md) | live SBC audio through io-media's Bluetooth reader (DSP decode, own resource manager); a stand-in for the Bluetooth stack's `/dev/wms/player1` |
| [audi_stream_receiver](src/audi_stream_receiver/README.md) | phone screen over USB (AOA): hardware H.264; phone app in [android/](android/), with an Android Auto mode ([docs/android-auto.md](docs/android-auto.md) §4.3) |
| [android_auto](src/android_auto/README.md) | Android Auto head unit, proof of connectivity: AOA, TLS, services; the phone's video counted, not shown |
| [runner](src/runner/README.md) | test menu: lists every binary in the card's `apps/` by file name; the main knob chooses, its press runs it (run.sh starts it) |

Which apps a card gets by default, their timeouts and arguments:
[sd/apps.conf](sd/apps.conf).

## Verified on the car

Tested on one unit (9411, HN+R_EU_AU_P1001, board E1):

- **Toolchain:** GCC 16 `-O2` C binaries and C++20 with a static libstdc++
  load and run; SH-4A LL/SC atomics (GCC's gUSA default loses updates under
  QNX).
- **Screen:** CPU drawing through the layer manager; OpenGL ES-CM 1.0 2D
  and 3D (all 9 pipeline probes pass). Heavy geometry or queued texture
  uploads can freeze the whole unit.
- **Video:** the DM642 DSP decodes MPEG-2, MPEG-4 and H.264 Baseline up to
  720×576 onto the screen, including a live stream served by the app's own
  resource manager (0.4–0.65 s lag). Skip frames keep it moving when the
  sender pauses.
- **Sound:** one audible output (`mixer:DSPOUT_TDM1_20_SAD`); it replaces
  the radio while playing and follows the volume knob. Live PCM is not
  solved.
- **Input:** knob and button messages decoded; the HMI still acts on them.
- **USB:** Android Open Accessory link to a phone; screen streaming as
  pictures (~3 fps) and as hardware H.264 (~25 fps, sharp text, follows
  the phone's rotation).

What is built but not yet confirmed is listed in each app's README and in
[docs/TODO.md](docs/TODO.md).

## Docs

| Doc | For |
|---|---|
| [native-apps.md](docs/native-apps.md) | developer guide: toolchain, build, running, every subsystem. Start here |
| [platform-notes.md](docs/platform-notes.md) | every confirmed platform fact, with its source |
| [gpu-results.md](docs/gpu-results.md) | GL measurements, budgets, the freezes |
| [cpp-apps.md](docs/cpp-apps.md) · [sdk/include-c++](sdk/include-c++/README.md) | C++ rules · how the C++ runtime is put together |
| [qnx-sh-fpu-div0.md](docs/qnx-sh-fpu-div0.md) | why a divide by zero is a fatal SIGFPE |
| [usb-stack.md](docs/usb-stack.md) | io-usb / libusbdi behaviour, AOA |
| [phone-mirroring.md](docs/phone-mirroring.md) | video decoder, display path, streaming options and results |
| [android-auto.md](docs/android-auto.md) | what an Android Auto receiver would need |
| [rear-camera.md](docs/rear-camera.md) | how the HMI shows the rear camera; what apps must not do |
| [bt-audio-path.md](docs/bt-audio-path.md) | how Bluetooth music reaches the speakers (BT stack, `/dev/wms`, io-media's `a2dp_player`); feeding that path from an app |
| [hbdsi.md](docs/hbdsi.md) | how the unit's processes call each other's services (servicebroker, HBDSI messages, SGAudioManagement) |
| [vehicle-data.md](docs/vehicle-data.md) | what the unit reads from the car's buses |
| [map-and-cluster.md](docs/map-and-cluster.md) | how the navigation map is drawn; how the cluster display is fed (DDP2, KOMO) |
| [power-onoff.md](docs/power-onoff.md) | who reboots / powers off the unit |
| [wifi-station-sd.md](docs/wifi-station-sd.md) | phone hotspot station from the SD card: telnet shell, file upload over inetd |

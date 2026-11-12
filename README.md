# Native C/C++ apps for the MMI 3G+

Plain C and C++20 programs for the Audi MMI 3G+ (QNX 6.3.2, Renesas SH7785 /
SH-4A, little endian), cross-compiled with Debian's stock SH4 GCC 16 inside
Docker. No QNX SDK: the toolchain links against stubs generated from the
unit's own libraries. How it works, how to write an app and every platform
rule: [docs/native-apps.md](docs/native-apps.md).

## Build and run

```sh
make sdk                        # once: the mmi-sdk Docker image (toolchain + QNX sysroot)
make sd                         # build + lay out build/sd/ with the default apps
make sd SD_APPS="hello5 hello9" # only these, in this order
make test                       # host unit tests
```

Copy the **contents** of `build/sd/` to the root of an empty FAT32 card,
boot the MMI fully, insert the card in SD slot 1. The apps run one after
another, each followed by a result screen; everything the run writes
(logs, screenshots, recordings) comes back in `var/` on the card, so
copying that one folder gets all of it. If the screen freezes, reboot the MMI:
hold **MENU + rotary knob + top-right soft key**. Details:
[docs/native-apps.md §4–5](docs/native-apps.md#4-build-system).

## Apps

Each app lives in `src/<app>/` with a README (what it does, how to run it,
what it showed on the car). Shared code is in [src/common/](src/common/).

| App | What it is | On the car |
|---|---|---|
| [hello](src/hello/README.md) | toolchain smoke test: writes `hello.txt` | works |
| [lmprobe](src/lmprobe/README.md) | layer manager without GL: colour bars straight into the VFB | works |
| [hello2](src/hello2/README.md) | OpenGL ES bring-up, full EGL/GL diagnostics | works |
| [hello3](src/hello3/README.md) | GPU canvas, fill-rate benchmark | works |
| [hello4](src/hello4/README.md) | 3D pipeline probes, scenes, triangle-rate benchmark | works up to 12 lit tori; 16 froze the unit |
| [hello5](src/hello5/README.md) | sound from an app through io-media | works on one output |
| [hello6](src/hello6/README.md) | phone-mirroring checks: benchmarks, DM642 video decode | done (5 of 10 clips play) |
| [hello7](src/hello7/README.md) | knob and button tester (`/dev/ipc/ch8`) | tool |
| [hello8](src/hello8/README.md) | C++ runtime and standard library checks | 19/19 |
| [hello9](src/hello9/README.md) | live H.264 stream through the DSP | works |
| [hello10](src/hello10/README.md) | 800-wide H.264 with its SPS cropped to 720, through the DSP | not yet run |
| [audi_stream_receiver](src/audi_stream_receiver/README.md) | phone screen over USB (AOA): hardware H.264 or JPEG; phone app in [android/](android/) | works |
| [android_auto](src/android_auto/README.md) | Android Auto head unit, proof of connectivity: AOA, TLS, services; the phone's video saved to the card | connects; video not shown |
| [pngondemand](src/pngondemand/README.md) | text layout → PNG, draws the run's status screens | tool |

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
| [gcc-sh4-treg-bug.md](docs/gcc-sh4-treg-bug.md) | the GCC 12 miscompile and the build canary |
| [usb-stack.md](docs/usb-stack.md) | io-usb / libusbdi behaviour, AOA |
| [phone-mirroring.md](docs/phone-mirroring.md) | video decoder, display path, streaming options and results |
| [android-auto.md](docs/android-auto.md) | what an Android Auto receiver would need |
| [rear-camera.md](docs/rear-camera.md) | how the HMI shows the rear camera; what apps must not do |
| [vehicle-data.md](docs/vehicle-data.md) | what the unit reads from the car's buses |
| [power-onoff.md](docs/power-onoff.md) | who reboots / powers off the unit |
| [bridge.md](docs/bridge.md) | laptop shell over USB (parked) |
| [TODO.md](docs/TODO.md) | what to verify next |

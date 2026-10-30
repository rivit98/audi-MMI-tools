# Native C apps for my MMI 3G+

Cross-compile plain C for the MMI 3G+ (QNX 6.3.2, Renesas SH7785 / SH-4A,
little endian) with Debian's stock SH4 GCC, packaged as a Docker image.
C++ works too: `shle-nto-g++` links GCC 16's libstdc++ statically
([`sdk/include-c++/README.md`](sdk/include-c++/README.md)).

| App | What it does |
|---|---|
| `hello` | On SD insert, writes `hello.txt` (`hello world`) to the card. Toolchain smoke test. |
| `lmprobe` | Layer manager test without OpenGL: replays showScreen's devctl sequence, fills the frame buffer with colour bars, logs every raw status. Passed on the car; its sequence lives on in `src/common/screen.c`. See [src/lmprobe/README.md](src/lmprobe/README.md). |
| `hello2` | OpenGL ES test. Logs full EGL/GL/gf diagnostics, runs a 20 s animation, and saves screenshots to the card. |
| `hello3` | GPU canvas: random-colour pixel blocks and a plasma effect, both rendered by the GPU from textures uploaded once, then a fill-rate benchmark. On-screen FPS counter. |
| `hello4` | 3D pipeline test: automatic pixel-readback probes (depth, culling, perspective, lighting, texture, fog, blend, stencil) shown as PASS/FAIL squares, then lit/textured/fogged 3D scenes and a triangle-rate benchmark. |
| `hello5` | Sound from our app. Steps (`hello5 <sd> <steps>`, default `mntv`) play a ping train that hello5 renders itself (`src/hello5/pulse.c`, written to `/tmp`) on the one audible output, io-media mixer `DSPOUT_TDM1_20_SAD`: with the radio (does it mix or replace?), with music playing and while the volume knob is turned, through the unit's `testapp-file`; and through hello5's own io-media client (`src/hello5/iom.c`, the devctls `testapp-file` uses) with its own volume control and queued files, every event timed. Step `g` is the earlier output survey: the bundled chime (`beep.wav`) in groups of 1–6 chimes, one group per output (mixer outputs and io-audio devices through QSA / `libasound`). |
| `hello8` | C++ test, built with `shle-nto-g++`: 18 checks of the language runtime (static init, exceptions, RTTI) and the standard library (containers, algorithms, strings, smart pointers, math, random, chrono, threads, condition variables, atomics, and gUSA vs LL/SC atomics under thread switches), each shown as PASS/FAIL with what failed. About 35 s, nothing to do. See [hello8](#hello8-make-sd-sd_appshello8). |
| `hello7` | Knob and button tester (a tool, out of the default run since 2026-10-25, because the MMI handles the buttons too and MENU/BACK bring its own screens over hello7's): decodes every `/dev/ipc/ch8` message (`src/hello7/keys.c`) and shows it live with a counter per control; bits no known control explains show as "unknown" with their byte, to name new buttons. `hello7 <sd> [seconds]`, default 60. Was hello5's stage 2. |
| `usb_tx_test` | A slow screen stream (JPEG pictures, up to 5 a second by default) from an Android phone over USB (Android Open Accessory). Shows the link state and the phone's screen full screen, logs everything to the card. (Text messages were dropped on 2026-10-25: [src/usb_tx_test/README.md](src/usb_tx_test/README.md).) Pairs with the phone app in `android/`. Runs until the SD card is pulled. See [usb_tx_test](#usb_tx_test-make-sd-sd_appsusb_tx_test). |
| `pngondemand` | Tool, not a test: renders a text layout (rectangles, text, word-wrapped paragraphs) to a PNG on the car. `run.sh` uses it to draw each status screen right before showing it. See [Screens](#the-test-run). |
| `hello6` | Checks the phone-mirroring assumptions ([docs/phone-mirroring.md](docs/phone-mirroring.md)): system/GPU info, CPU/VFB/texture-upload/LZ4 benchmarks, then ten test videos through the unit's DM642 decoder on a layer manager video displayable. The default run plays only the videos: about 8 minutes, driver watching. See [hello6](#hello6-make-sd-sd_appshello6). |

```sh
make sdk               # once: build the mmi-sdk Docker image (toolchain + QNX sysroot)
make sd                # compile + assemble build/sd/: every test app, run in turn
make sd SD_APPS="hello hello2"  # only these, in this order
# copy the CONTENTS of build/sd/ to the root of a FAT32 SD card, insert in car
```

**Install on an SD card:** after `make sd`, copy everything *inside*
`build/sd/` to the root of an empty FAT32 card. With the MMI fully
booted, insert it in SD slot 1 and leave it in until the "all done"
screen has gone. The card carries its own short user guide,
[`sd/README.md`](sd/README.md), copied there as `README.md`. It explains
each test, what you should see, the verdicts, and which files to bring
back.

**Status:** all five build, and the ELFs are structurally identical to the stock
binaries and to qnx-audi's `qcc`-built binary (see below). **None has run
on the car yet.** Run `hello` first: it isolates the toolchain from the
graphics stack.

`hello5`'s QSA struct ABI (`sdk/include/sys/asoundlib.h`) has no QNX header to
copy, so it was recovered by disassembling the device's own `libasound.so.2`
in IDA (Hex-Rays has no SH4 target; it is read from the SH4 disassembly). The
`snd_pcm_channel_params_t` size (752 B, from the `devctl` dcmd `0xC2F04134`)
and the `channel`/`mode`/`format`/`frag_size`/`stop_mode`/`frags_max` offsets
are confirmed from the code and locked by `_Static_assert`s in that header;
`start_mode`/`frags_min` sit in the gaps and are inferred. The enum values come
from the unit's io-audio, and the setup read-back has its own layout (format
`@0x08`). On 12.10 v2 the params were accepted and 15 clips were written in
full. `prepare` returns `EBUSY` while the previous chime is still playing.
Whether it was audible is still unconfirmed (docs/platform-notes.md §9).

---

## Is the compiler "universal"?

Partly. A C compiler's output splits into two layers:

| Layer | Decided by | Same for Linux and QNX on SH4? |
|---|---|---|
| Instructions, registers, calling convention, struct layout, FPU mode | CPU + ABI | **Yes.** Both follow the Renesas SH ELF ABI (`-m4 -ml`). |
| Headers (`FILE`, `errno`, `off_t`, …) | OS / libc | No → use **openqnx headers** (`-nostdinc`) |
| Program startup (`_start` → `main`) | OS | No → use **QNX `crt1.S`** etc. from openqnx |
| System calls | OS | No, but we never make one directly. Every call goes through the device's `libc.so.2`. |
| Dynamic loader + ELF metadata | OS | No → interpreter `/usr/lib/ldqnx.so.2`, SysV hash, non-PIE, `BIND_NOW` |
| Libraries to link against | OS | No → **stubs of the real MMI `.so` files** |

So we use only the code generator from `sh4-linux-gnu-gcc` and replace every
OS-specific input. The compiler never sees glibc, Linux headers or Linux
startup code.

### How QNX libraries are used without a QNX SDK

The firmware's `.so` files (`dev/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/…`)
can't be linked against directly: mkifs stripped them to 3 section headers,
so they have no `.dynsym` section for `ld` to read. Their **dynamic segment is
intact** (QNX's own loader needs it), so `sdk/mkstubs.py` walks
`PT_DYNAMIC → DT_HASH/DT_SYMTAB/DT_STRTAB/DT_SONAME` through the program
headers. From that it generates a stub library with the same SONAME and
symbol names, and the same sizes for data objects (the linker needs those for
`R_SH_COPY`, e.g. `errno`). We link against the stub. On the car, the real
library is loaded.

| Stubbed library | SONAME | Exported symbols | Notes |
|---|---|---|---|
| `proc/boot/libc.so.2` | `libc.so.2` | 1722 | also exports GCC's SH helpers |
| `usr/lib/libm.so.2` | `libm.so.2` | 383 | |
| `usr/lib/libgf.so.1` | `libgf.so.1` | 291 | QNX Graphics Framework |
| `usr/lib/libEGL.so.6` | `libEGL.so.6` | 36 | **NVIDIA** EGL 1.2 (`NvEglRegClientApi`) |
| `usr/lib/libGLES_CM.so.6` | `libGLES_CM.so.6` | 148 | GLES Common (reports `OpenGL ES-CM 1.0`; float + fixed) + `egl*` forwarders |

---

## Findings: toolchain

1. **openqnx has no compiler.** `dev/openqnx` (QNX **6.4.1** source leak)
   has no GCC or binutils. It provides the headers (`lib/c/public`,
   `services/system/public`) and the SH startup code
   (`lib/c/startup/sh/{crt1,crti,crtn}.S`, `crtbegin/crtend.s`).
2. **Debian ships a usable SH4 compiler:** `gcc-16-sh4-linux-gnu` 16.2 and
   binutils 2.47 in sid. (bookworm's GCC 12.2 also works, but only at `-O0`:
   see [`docs/gcc-sh4-treg-bug.md`](docs/gcc-sh4-treg-bug.md).) GCC 14+
   predefines `_FILE_OFFSET_BITS=64` / `_TIME_BITS=64` for Debian's 64-bit
   `time_t`; the wrapper undefines both, or `open()` becomes `open64()`.
3. **The headers support modern GCC.** `sys/compiler_gnu.h` uses
   `__builtin_va_*` for GCC ≥ 3. GCC only needs QNX's macros:
   `-D__QNX__ -D__QNXNTO__ -D__SH__ -D__LITTLEENDIAN__`, with the Linux
   macros undefined.
4. **The device libc exports GCC's SH helpers itself:** `__set_fpscr`,
   `__fpscr_values` and `__sdivsi3_i4`.
5. **FPU mode:** `crt1.S` calls `__set_fpscr(0xc0400)`, which sets PR=1
   (double precision), DN=1, and enables the divide-by-zero trap. That's
   exactly GCC's `-m4`. GCC switches FPU modes inline and doesn't need
   `__fpscr_values`.
6. **The init/fini array runners don't exist anywhere.** `crt1.S` calls
   `_preinit_array`, `_init_array` and `_fini_array`. They lived in QNX's
   proprietary static libc. `sdk/crtarray.c` implements them.
7. **Two linker-reserved symbols had to be dropped.** `libc.so.2` exports
   `_DYNAMIC` and `_GLOBAL_OFFSET_TABLE_`, so `mkstubs.py` leaves them out.
8. **Debian's assembler marks every object `sh4a` by default.**
   `-Wa,--isa=sh4` makes it reject SH-4A-only opcodes and label the ELF `sh4`.
9. **64-bit alignment: QNX 6.4.1 ≠ 6.3.2.**
   - openqnx's `compiler_gnu.h` declares `_Int64t`/`_Uint64t` with
     `__aligned__(8)`.
   - The MMI's 6.3.2 libraries were built without it. Their
     `gf_surface_info_t`, which holds a `_Uint64t`, is 68 bytes, not 72.
   - `build-sysroot.sh` strips the attribute, so `int64_t`/`uint64_t` are
     4-byte aligned. That matches both QNX 6.3.2 and GCC's own SH default
     (`-mdalign` off).
   - libc's own structs (`stat`, `stat64`) are written so this doesn't
     affect them. `gf`'s structs aren't. The static asserts in `gf.h`
     caught the mismatch.
10. **C++ needs its own runtime.** The car's `libstdc++.so.5` (GCC 3.3)
    and Dinkumware `libcpp.so.4` don't fit GCC 16's headers, so
    `shle-nto-g++` links Debian's `libstdc++.a` statically. It is built for
    glibc, so a header overlay and a small runtime shim (TLS-free exception
    globals, `_dl_find_object` for the unwinder, 64-bit-`time_t` clocks,
    `condition_variable` rebuilt with QNX's layout) sit between it and
    QNX's libc. Iostreams, `<format>` and `thread_local` don't work, and
    atomics are probably not atomic under preemption (gUSA). Details:
    [`sdk/include-c++/README.md`](sdk/include-c++/README.md).
11. **The SD autorun chain** is in the toolkit already:
    1. `proc_scriptlauncher` finds `copie_scr.sh` (XOR-encoded by
       `builder/encoder.py`).
    2. That runs `core/copie_scr_plain.sh`.
    3. That runs `exec ksh ./run.sh $SDPATH`.
    4. `run.sh` copies `bin/*` to `/tmp/mmi_apps/` and runs the test apps
       one after another, logging to `var/run.log` (see
       "The test run").

## Findings: graphics stack

How the MMI draws (from qnx-audi + P1001 binaries):

```
MMI3GApplication / your app
   │ devctl /dev/layermanager       ← layermanagerV2 (-c /etc/layermanagerV2_nvidia.cfg)
   │   register displayable (cid, w, h, flags) → get its VFB (gf surface sid)
   │
   ├─ libgf.so.1   gf_dev/display/layer/surface  → io-display + devg-nv.so (NVIDIA)
   └─ libGLES_CM.so.6 / libEGL.so.6 (NVIDIA)
        EGLNativeDisplayType = gf_dev_t,  EGLNativeWindowType = gf_3d_target_t
        gf_3d_target_create(layer = topmost, surfaces = { VFB })
```

- **Layer manager protocol (unchanged in P1001).** All six `devctl` codes
  qnx-audi reversed are present in P1001's `layermanagerV2`,
  `MMI3GApplication`, `MMI3GMedia` and `MMI3GNavigation`:

  | Code | Meaning (from layermanagerV2's log strings) |
  |---|---|
  | `0xC0140500` | `REGISTER_DISPLAYABLE(cid, w, h, flags)` |
  | `0xC0100503` | `GET_VFB` → `{sid, uid, alpha_sid}` |
  | `0x80040504` | `UPDATE_VFB` |
  | `0x800C0505` | `SHOW_DISPLAYABLES` |
  | `0xC00C0506` | `CHECK_VERSION` |
  | `0x80040501` | `UNREGISTER_DISPLAYABLE(cid)` (qnx-audi: unknown) |

  A rejected request comes back as `devctl() == EINVAL` plus `<ERR>` lines
  in the slog. Displayables outlive their client (closing the fd doesn't
  unregister), so apps unregister their cid before registering, as
  showScreen does.

  **The VFB is double-buffered:** `GET_VFB` returns the back surface and
  `UPDATE_VFB` flips front/back. So the sid changes with every update, and a
  CPU-drawing client must call `GET_VFB` again after each `UPDATE_VFB`.
  Otherwise every other update shows a never-written surface (noise). See
  docs/platform-notes.md §3.

- **Text commands for debugging.** Writing to `/dev/layermanager` accepts:
  - `s-shot <display> <file>`: whole-screen snapshot
  - `d-shot <cid> <file>`: snapshot of one displayable
  - `da-shot <cid> <file>`: snapshot of a displayable's alpha plane
  - `v-level` / `s-level`: unknown
  
  The file type follows the extension, via `img_codec_{png,bmp,jpg,gif}.so`.
- **`gf` headers aren't public, so I reconstructed them**
  (`sdk/include/gf/`):
  - signatures come from the QNX 6.4.1 GF docs
  - struct layouts and `gf_format_t` values come from the STABS debug info
    in qnx-audi's `dbg-shle.elf` (built with QNX 6.3.2 headers)
  - `GF_ERR_*`, `GF_DEVICE_INDEX(0)` (= NULL) and
    `GF_LAYER_ATTACH_PASSIVE` (= 1) come from disassembling its switch tables
    and call sites

  The 6.4 docs are **wrong for 6.3.2**: `gf_dev_info_t` has `reserved[6]`
  instead of vendor/device ids, and `vaddr` comes before `paddr`.
- **GLES/EGL headers** are standard Khronos (Debian `libgles-dev`/
  `libegl-dev`), compiled with `EGL_NO_PLATFORM_SPECIFIC_TYPES` so native
  types are `void*`.
- **Your firmware is newer than qnx-audi's test cars.** Their binary needs
  `libGLES_CM.so.1`, but P1001 ships `libGLES_CM.so.6` (NVIDIA). We link the
  `.so.6` that's actually there.
- qnx-audi's log from a real car shows an **800×480** display and the
  bring-up sequence that `src/common/gfx.c` reproduces.

### Our binaries vs stock

| | stock `/bin/cat` | qnx-audi (`qcc`) | our `hello2` |
|---|---|---|---|
| Type / base | `EXEC` @ `0x08040000` | `EXEC` | `EXEC` @ `0x08040000` |
| Interpreter | `/usr/lib/ldqnx.so.2` | same | same |
| Symbol hash | SysV | SysV | SysV |
| Bind | — | `FLAGS_1 NOW` | `FLAGS_1 NOW` |
| Data imports | — | `R_SH_COPY` (`errno`, …) | `R_SH_COPY` (`errno`, `_Stdout`, …) |
| e_flags | `0x8` (sh3e) | — | `0x9` (sh4) |
| `PT_GNU_STACK` | no | — | **yes** (GNU ld always emits it) |

---

## Layout

```
Dockerfile            mmi-sdk image: debian sid + gcc-16-sh4-linux-gnu + sysroot
Makefile              host entry point (sdk / all / check / sd / shell / clean)
sdk/
  shle-nto-gcc        compiler wrapper: QNX headers, crt objects, link flags;
                      as shle-nto-g++ (a link) the C++ compiler
  qnx-elf-fix.py      run after every link: GNU ELF features ldqnx lacks
                      (STB_GNU_UNIQUE -> GLOBAL), fails on IFUNC / TLS
  include-c++/        C++: libstdc++ header fix-ups for QNX; README.md
  cxxrt.c             C++ runtime shim: glibc pieces libstdc++.a expects
  cxxrt-std.cpp       C++ runtime shim: libstdc++ members rebuilt for QNX
  build-sysroot.sh    runs in `docker build`: headers, crt, stubs, 6.3.2 patch
  mkstubs.py          stripped device .so → link-time stub (.S)
  fonts/              Terminus Font 8x16 / 16x32 console fonts (SIL OFL, OFL.txt)
  crtarray.c          _preinit_array/_init_array/_fini_array for crt1.o
  include/gf/         reconstructed gf.h / gf3d.h (QNX 6.3.2 layouts)
  canary.c            build-time check for the GCC miscompile (docs/gcc-sh4-treg-bug.md)
  mkbeep.py           renders src/hello5/beep.wav (the knob-detent chime)
  include/sys/        reconstructed QSA asoundlib.h (from the unit's libasound)
                      and USBDI usbdi.h (from the unit's libusbdi, see usb_tx_test)
  mkfont.py           PSF font -> C glyph table (the apps' on-device text)
docs/
  platform-notes.md   what the car has confirmed: GPU limits, layer manager,
                      vsync, capture, input, audio ABI. Start here.
  cpp-apps.md         writing apps in C++: building, what's safe, the
                      threads/atomics rules, what isn't available
  qnx-sh-fpu-div0.md  why a divide by zero is a fatal SIGFPE on this platform
  gcc-sh4-treg-bug.md GCC 12.2's -O1+ `a == 0 && b == 0` miscompile (fixed in 16)
src/
  common/             shared by the apps (src/<app>/ is only that app's logic)
    log.c             SD log, fsync per line
    lmgr.c            /dev/layermanager client (devctl + text commands)
    gfx.c             gf + EGL bring-up / teardown (port of qnx-audi Screen)
    img.c             BMP / PNG writers over the vendored stb_image_write.h
    jpeg.c            JPEG decoding over the vendored stb_image.h
    quit.c            clean-stop sources: timer, signals, SD removal, watchdog
    hud.c             seven-segment FPS counter + rectangles
    text.c            UTF-8, word wrap, glyph drawing (Terminus fonts)
    screen.c          CPU-drawn full-screen layer-manager displayable (VFB)
    proc.c            start the unit's own tools from a threaded app
  hello/hello.c
  lmprobe/main.c      showScreen's layer manager sequence, colour bars, no GL
  hello2/main.c
  hello3/main.c       scenes, timing, fill-rate table
  hello4/main.c       3D scenes, timing, triangle-rate table
  hello4/mesh.c       cube / torus / ground grid (indexed, client-side arrays)
  hello4/probe.c      glReadPixels pass/fail checks of the 3D pipeline
  usb_tx_test/        AOA receiver: aoa.c (USB), proto.c (frames), vasm.c
                      (picture reassembly), video.c (decode, acks), ui.c
                      (screen layouts), model.c
  pngondemand/        layout -> PNG: layout.c (the layout language), main.c
                      (PNG via common/img.c); README.md
  hello5/             sound: main.c (steps), pulse.c (ping trains), iom.c
                      (io-media client), h5ui.c (screens)
  hello6/             mirroring checks: main.c (phases), sysinfo.c, bench.c,
                      video.c, label.c (text screens), texts.h (their text +
                      the clip list), lz4mini.c
  hello7/             knob/button tester: keys.c (ch8 decoding), main.c
  hello8/             C++ test: checks.cpp (the checks, host-tested too),
                      main.cpp (log + screen)
  hello*/README.md    what each hello app does, in short
media/                hello6's test videos: mkclips.sh (ffmpeg, mmi-media image)
tests/                host unit tests (make test)
android/              usb_tx_test sender app (Kotlin, Gradle in docker/Dockerfile)
tools/aoa_host.py     desk stand-in for the MMI side (make aoa-host)
sd/run.sh             launched by copie_scr.sh on SD insert: runs the test apps
                      (@APPS@ and screen times filled by make)
sd/screens.sh         run.sh's status screens as pngondemand layouts (sourced)
sd/apps.conf          per-app timeout, arguments, description (run.sh + screens)
sd/README.md          short user guide, copied to the card as README.md
build/                output (gitignored); build/sd/ = SD card root
```

Use the wrapper like gcc in your own builds:

```sh
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src mmi-sdk \
    shle-nto-gcc -O2 -o app app.c -lGLES_CM -lgf
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src mmi-sdk \
    shle-nto-g++ -O2 -o app app.cpp -lgf
```

## Testing on the car

General procedure:
1. FAT32 card. Copy `build/sd/*` to the root.
2. Turn the ignition on and wait for the MMI to boot fully. Insert the card.
3. A start screen comes first. Then each app gets a "Next" screen (what it
   does and what you should see) and a result screen. The default run
   is hello8, hello5, hello6 (about 8 min), then usb_tx_test, which runs until
   the card is pulled: that is the end of the run, so its result screen
   and the "all done" screen never appear. With an `SD_APPS` list that
   doesn't end in usb_tx_test, wait for "all done" before pulling.

If neither the apps' output nor `var/run.log` appears,
`copie_scr.sh` didn't run.

### The test run

`run.sh` (ksh, no native runner) does this for each app in `SD_APPS`:

0. **Start screen ("MMI 3G+ native app tests")** for 5 s, once: what the run is,
   and don't pull the card before "all done".
1. **Screen "Next: <app>"** for 5 s: its title and what you should see or
   do, its arguments and timeout, and the run order. This is the pause
   before the app starts.
2. **The app**, with stdout and stderr in `var/<app>_out.log`. Three `=== `
   lines are appended at the end: exit code, run time and verdict. A watcher
   sends TERM when the app runs over its timeout, and KILL 15 s later.
   Then `sloginfo` goes to `var/<app>_slog.txt` and core files are moved to
   `var/` (see "When an app crashes").
3. **Screen "<app>: <verdict>"** for 8 s, with what the verdict means, the
   exit code, run time and FAIL line count, and where its logs are.

The run order on every screen shows the verdicts so far. Then a final
screen "k of n apps OK" for 15 s. The verdicts, with exit
code and run time, are in `var/summary.txt`.

| Verdict | Meaning |
|---|---|
| `OK` | exit 0, no line containing `FAIL` |
| `WARN` | exit 0 but `FAIL` lines. The graphics apps exit 0 even when `gfx_init` fails. |
| `FAIL` | non-zero exit code |
| `CRASH` | killed by a signal (exit code − 128) |
| `TIMEOUT` | stopped by the watcher |
| `MISSING` | the binary isn't in `bin/` |

Per-app timeouts, arguments, titles and on-screen descriptions are in
[`sd/apps.conf`](sd/apps.conf), which `make sd` also copies to the card.
hello5 runs steps `mntv` there. The screen times are
`PRE_S`/`POST_S`/`FINAL_S` in the Makefile.

**Screens.** They are shown with the toolkit's `core/bin/showScreen`
(DrGER's tool, already proven on the car), as
`showScreen -t <seconds> <png>`. `-t` shows the image and blocks until
the time is up. showScreen can only show a PNG, so each screen is drawn
on the car when it is needed, nothing is pre-rendered:

1. a `screen_*` function in [`sd/screens.sh`](sd/screens.sh) prints the
   screen's layout (`screen_start`, `screen_next`, `screen_result`,
   `screen_done`) to `/tmp/mmi_apps/screen.layout`;
2. `bin/pngondemand` renders it to `/tmp/mmi_apps/screen.png`;
3. `showScreen -t` shows that.

The rendering time counts towards the screen's time. If any step fails,
or showScreen returns early, `run.sh` sleeps the rest of the time, so the
timing doesn't change. pngondemand's errors go to `var/run.log`,
showScreen's output to `var/showscreen.log`.

**pngondemand** renders a text layout (one command per line: `bg`,
`rect`, `text`, word-wrapped `para`, `footer`, ...) to a PNG:
`pngondemand [-s] <layout|-> <out.png|->`. The layout language, options
and an example are in [`src/pngondemand/README.md`](src/pngondemand/README.md);
`pngondemand -h` prints a summary. The PNG is written by `common/img.c` over
the vendored [stb_image_write](https://github.com/nothings/stb) (public domain / MIT),
as the SDK has no zlib or libpng.

Verified on the car (video of the 2026-10-11 run): the screen goes back to
the HMI when `showScreen -t` ends, and a hello app takes the screen right
after it. If a screen ever stays up, the MMI reboot key combo clears it.

### Stopping an app

`hello` writes its file and exits immediately. hello2–hello4 always
leave through the same teardown (`gfx_shutdown`), which hands the screen
back to the HMI. The log ends with `stop requested: <reason>` and
`exit (stop reason: …)`. The ways to stop them (`src/common/quit.c`):

| How | Notes |
|---|---|
| Wait | The normal end: 20 s for hello2, 3 × 10 s for hello3, 4 × 10 s for hello4 (with no arguments; `sd/apps.conf` gives it 3 × 10 s of scenes and 5 × 10 s of stress, ~85 s). |
| **Pull the SD card** | Checked once per second. The card is gone, so the log and screenshots are lost after that point. |
| `slay hello3` / `kill -TERM <pid>` | From a shell (telnet or serial), if you have one. SIGTERM, SIGINT and SIGHUP all work. A second signal forces `_exit`. |
| Watchdog | Run time + 40 s, in case GL or the layer manager hangs. |

After any stop request the app has 10 s to finish its teardown. If it is
still running after that, it is ended with `_exit()`. **If the screen then
stays frozen, reboot the MMI with the key combo.** Signals go to a
`sigwait()` thread, not a handler, so they never interrupt a GL or gf call
in the render thread.

### When an app crashes

The unit runs the customised QNX `dumper` (*"Audi 3G+ dumper"*):

    /usr/sbin/dumper -m -w -z 1 -c -d /HBpersistence/core -d /mnt/persistence \
                     -f io-pkt-v4-hc -f devc-serusb -f io-usb

On a crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE, abort and so on) it does the
following:
- **Writes a core file** `<name>.core*`, gzip level 1, to
  `/HBpersistence/core`. That is a symlink to
  `/mnt/efs-persist/syslog/core` on **flash**. If the directory isn't
  accessible it falls back to `/mnt/persistence`, then `/tmp`.
  - `-f` lists processes that get a full memory dump. Others, ours
    included, get a reduced one (`-m`).
  - `-c` checks free memory (`/dev/checkFreeMem`) before dumping.
- **Logs to slog** a `run fault pid … signal … ip 0x…` line and a
  backtrace (`BT …`), using `libbacktrace`.

After every app, `run.sh` saves `sloginfo` to `var/<app>_slog.txt`. It
moves any `<app>.*` core files from those three directories to `var/`, so
they don't use up the unit's flash. An exit code above 128 in
`var/<app>_out.log` means the app was killed by signal (code − 128).

All apps are built with `-g`, unstripped, at a fixed load address
(ET_EXEC), so a crash address resolves directly to a source line:

```sh
make addr APP=hello3 ADDR="0x08043270 0x080431a4"
#   key_thread at /src/src/common/quit.c:83
```

Addresses inside a shared library (outside `0x0804xxxx`) need that
library's load base, which is in the core file. Modern GDB has no QNX/SH
support, so cores are best read with `readelf -n`/`-l` plus addr2line.

### hello (`make sd SD_APPS=hello`)

`hello.txt` containing `hello world` means the toolchain works. Otherwise,
`var/hello_out.log` has the loader or binary error. If the loader rejects
the binary, the first suspects are:
- `PT_GNU_STACK` (fix: a custom linker script with `PHDRS`, modeled on
  openqnx `lib/c/startup/sh/nto.link`)
- the `sh4` e_flags

### hello2 (`make sd SD_APPS=hello2`)

On screen: the HMI is replaced for about 20 s by the following:
- a background that slowly cycles through hues
- a red/green/blue/yellow square in the centre, rotating at 90°/s
- a white block bouncing along the top
- a yellow progress bar at the bottom

Afterwards the app hands the screen back the way qnx-audi does. **If the
screen stays frozen on the last frame, reboot the MMI with the key combo.**
qnx-audi needed that too.

Files on the card:

| File | Proves |
|---|---|
| `hello2.log` | Every bring-up step with its result, then the diagnostics (below) and per-second FPS. A failure ends with a `FAIL <call>: <error name>` line. |
| `hello2_glread.bmp` | `glReadPixels` of our frame at t=5 s: **GL rendered.** |
| `hello2_dshot.png` | Layer manager snapshot of our displayable (cid 40): **rendering reached the VFB.** |
| `hello2_sshot.png` | Snapshot of the whole display: **our layer is what's actually visible.** |
| `var/hello2_out.log` | stdout/stderr, exit code, verdict (`var/run.log`: `run.sh` itself). |

Diagnostics in `hello2.log`:
- **Layer manager:** version.
- **gf:** device description, display mode, refresh, number of layers,
  every pixel format of the top layer, and VFB surface info.
- **EGL:** version, vendor, client APIs, extensions, and every config
  (RGBA/depth/stencil/visual/caveat).
- **GL:** vendor, renderer, version, extensions, limits (texture size and
  units, viewport, lights, stacks, point/line ranges) and RGBA/depth/stencil
  bits.

hello2 and hello3 stop cleanly in several ways; see "Stopping an app"
below.

### hello3 (`make sd SD_APPS=hello3`)

Run hello2 first: hello3 uses the same bring-up but logs only the GL
renderer and version, not the full diagnostics.

The CPU builds two textures once at start (256×256 random colours,
128×128 tileable plasma). After that each frame is a few quads plus a
texture matrix. Every pixel comes from the GPU (texture sampling,
filtering, blending), with no per-frame upload over PCI. Only GLES 1.0
Common entry points are used (`glTexParameterf`, `glTexEnvf`;
power-of-two textures). The device runs ES-CM 1.0, and its NV18 GPU is
fixed-function.

On screen, three scenes of 10 s each (`hello3 <sd> <seconds>` changes
the length):

| Scene | What you see | How the GPU makes it |
|---|---|---|
| 1 `pixels` | A full-screen grid of random-coloured blocks that changes every 100 ms. The blocks shrink 32 → 16 → 8 → 4 px. | Noise texture with `GL_NEAREST`, so each texel is one block. A random whole-texel jump of the texture matrix gives a new canvas. |
| 2 `plasma` | Smoothly flowing rainbow patterns. | Three layers of the plasma texture, each scrolled, rotated and scaled by its own texture matrix, then alpha- and additively blended. |
| 3 `fill` | The plasma getting busier. The yellow number is the layer count: 1, 2, 4, 8, 16, 32. | N full-screen textured, blended layers per frame. |

All scenes show the frames from the previous second (green, top left)
and a scene progress bar (bottom).

Files on the card:

| File | Contents |
|---|---|
| `hello3.log` | Bring-up, a per-second `fps` / `worst frame` line, a per-scene summary, and the **fill-rate table** at the end |
| `hello3_pixels.png`, `hello3_plasma.png` | Layer manager screenshots taken mid-scene. Screenshots are skipped during `fill` because they would skew its timing. |

Reading the fill-rate table (`N layers: frames, ms, fps, Mpixel/s`):
- **fps stays near 60 while N grows:** swaps are locked to the 60 Hz
  refresh, and the GPU has fill rate to spare.
- **fps falls roughly as 1/N:** the GPU is fill-bound. The Mpixel/s
  column then shows its textured, blended fill rate.
- **fps is low even at N=1:** the cost is per frame, not per pixel.
  Suspects are `gfx_swap`'s `glFinish` + `eglWaitGL`, or the layer
  manager compositing.

### hello4 (`make sd SD_APPS=hello4`)

Run hello3 first: hello4 uses the same bring-up and adds the 3D half of
ES-CM 1.0. The device's `libGLES_CM.so.6` has no buffer objects
(`glBindBuffer` is not exported), so meshes are drawn from client-side
arrays with `glDrawElements`. The EGL config already asks for a 16-bit
depth buffer (`choose_config` in `gfx.c`).

**Probes first.** Before anything is shown, `probe.c` draws small test
scenes into the back buffer, which is never swapped, and reads pixels back
with `glReadPixels`. It compares them with what the GL spec requires,
within ±40 per channel to allow for an RGB565 surface. Each read and
verdict is logged. The verdicts stay on screen during every scene as a
row of squares, top right: **green = pass, red = fail, grey = skip**.

| # | Probe | Checks |
|---|---|---|
| 1 | `clear` | `glClear` colour read back at the centre and both corners. If this fails, the other probes are meaningless. |
| 2 | `depth` | `GL_LESS` rejects a farther quad drawn later. `glDepthMask(GL_FALSE)` stops depth writes. |
| 3 | `cull` | CW quads are culled and CCW quads drawn. `glFrontFace(GL_CW)` reverses that. |
| 4 | `persp` | `glFrustumf`: quads at z = −2 and −4 cover exactly the predicted screen area. This is also a check of float arguments across the library ABI. |
| 5 | `light` | Directional diffuse light at N·L = 1, 0.707 and < 0 gives red 255, 180 and black. |
| 6 | `texture` | All four texels of a 2×2 `GL_NEAREST` texture, `GL_REPLACE`. |
| 7 | `fog` | Linear fog: fully fogged, half and none. |
| 8 | `blend` | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` at α = 0.5. |
| 9 | `stencil` | Drawing only where a stencil mask was written. `gfx.c` now asks for an 8-bit stencil config (the driver has them), so this should run rather than SKIP. |

Then four scenes of 10 s each (`hello4 <sd> <seconds>` changes the length;
what runs is selectable, see "Options" below):

| Scene | What you see | Exercises |
|---|---|---|
| 1 `cube` | A big multicoloured cube and a small one orbiting through it, on navy. | Perspective, depth buffer (the cubes cut through each other cleanly), back-face culling, Gouraud colour, matrix stack. |
| 2 `light` | A gold torus with a moving blue point light (small blue cube). Faceted in the second half. | Two lights (directional + attenuated point), specular, `GL_SMOOTH` → `GL_FLAT`. |
| 3 `world` | A checkerboard ground fading into a blue sky, a ring of spinning wooden crates and a floating torus. The camera circles. | Mipmapped trilinear textures, texture × lighting (`GL_MODULATE`), linear fog, perspective-correct texturing. |
| 4 `stress` | A grid of 1, 2, 4 … 64 lit tori (yellow number = count). | Vertex/lighting throughput: 1024 triangles per torus, up to 65 536 per frame. |

What to look for: in `cube`, any flicker or a cube showing through the
other means a depth problem. In `world`, shimmering or moiré in the far
checkerboard means mipmapping is not used.

Files on the card:

| File | Contents |
|---|---|
| `hello4.log` | Bring-up, GL limits (depth/stencil bits, max lights), every probe read with its verdict, per-second fps, per-scene summary, and the **triangle-rate table** at the end |
| `hello4_cube.png`, `hello4_light.png`, `hello4_world.png` | Layer manager screenshots taken mid-scene (none during `stress`) |

The triangle-rate table (`N tori = T tris: frames, ms, fps, ktris/s`)
reads like hello3's fill-rate table. If fps stays near 60 while N grows,
the GPU has headroom. If fps falls as 1/N, the ktris/s column is the
transform and lighting rate for lit, untextured, indexed triangles. The
tori are small on screen, so fill rate is not what limits it.

**The stress scene froze the whole unit on 2026-10-11** (picture, knobs and
radio volume, for minutes; no watchdog fired) while drawing 8 tori, after
probes and scenes 1–3 had all passed. See
[`docs/gpu-results.md`](docs/gpu-results.md). The 2026-10-19 run used only
the stress scene, built to find out when and why. It passed 8 tori for 60 s
and 12 tori, then froze the unit on the first 16-tori frame (gpu-results
§8). The default stress steps now stop at 12 (16, 32 and 64 are commented
out in `opts_default`). On 2026-10-21 everything (probes, three scenes,
stress 1–12 tori) passed in one run (gpu-results §9), so hello4 has left
the default run. Its `sd/apps.conf` line is kept, commented out:

```
hello4 <sd> step=10 trace=1
```

Options (any order, after `<sd>`):

| Option | Meaning | Default |
|---|---|---|
| `<n>` | seconds per scene (cube/light/world), and for the whole stress scene when there is no `step=` | 10 |
| `scenes=pclws` | what runs, in this order: `p` probes, `c` cube, `l` light, `w` world, `s` stress | all |
| `tori=1,2,8:60` | stress steps: torus counts, each with optional own seconds | 1,2,4,8,12 |
| `step=<s>` | seconds of a stress step without its own | scene time ÷ steps |
| `trace=0/1/2` | stress only. 1: one fsync'ed line per frame after its swap, `f<frame> n=<tori> draw=<ms> swap=<ms>`. 2: also `f<n> draw` / `f<n> swap` before each. Each line costs ~12 ms on the card, so fps drops. | 0 |

A bad option is a `FAIL` line and exit 2, so nothing is drawn. Each stress
step logs its start with triangles per frame and free RAM, and the
per-second line now includes `free=<MB>` (the size of `/proc`), to catch a
driver leak before a freeze. After a freeze, the last `f<n>` line in
`hello4.log` is the last frame that finished.

### hello8 (`make sd SD_APPS=hello8`)

The first C++ app (`src/hello8/README.md` lists every check). Each check
records the first expectation that failed (`line N: expr`); while
everything passes it notes a measurement instead: time per throw, sort
and map timings, `sleep_for` accuracy, mutex and `fetch_add` throughput.

- **Before each check** its name goes to the log (`check exceptions ...`),
  so after a crash or a freeze the last such line names the culprit. The
  checks that everything else relies on come first (static init, then
  exceptions); the threaded ones come last.
- **After `main` returns**, a static object's destructor appends `static
  destructor ran at exit` to `hello8.log`: the `__cxa_atexit` path.
- **The log's first lines** give the GCC version, `__cplusplus`, and the
  sizes of `std::mutex`, `std::condition_variable` and their pthread types
  as built (QNX's layouts: 8 bytes each, not glibc's 24/48).
- **The likeliest failure is `atomics`:** lost `fetch_add`s between 4
  threads would confirm that GCC's gUSA atomics aren't atomic under QNX
  (`sdk/include-c++/README.md`). It is a count, not a crash, and the other
  checks don't depend on it.
- **The same checks run on the build host** with glibc's libstdc++, under
  ASan/UBSan (`make test`, `tests/test_hello8.cpp`). All pass, so the
  expectations are right and a FAIL on the car is the car's runtime.
- **2026-10-25 on the car:** it didn't load (`unknown symbol:
  _ZNSs4_Rep20_S_empty_rep_storageE`): a `STB_GNU_UNIQUE` symbol from
  `libstdc++.a`, which QNX 6.3's loader doesn't know. `shle-nto-gcc` now
  rewrites such symbols after the link (`sdk/qnx-elf-fix.py`).
- **2026-10-28 on the car: 17 of 17 passed**, the static destructor ran,
  exit 0. `atomics` was too short (under 2 ms per thread) for the threads
  to interleave, so the gUSA question is still open.
- **`atomics stress`** (new, not yet on the car) settles it: 4
  round-robin threads for 1.5 s each with a plain load/store control,
  GCC's gUSA `fetch_add`, and `fetch_add` built with SH-4A LL/SC
  (`src/hello8/llcs.cpp`, `-matomic-model=hard-llcs`). Timings are in
  [src/hello8/README.md](src/hello8/README.md).

`hello8 <sd> [seconds] [checks]`: the results stay up for `seconds`
(default 20); `checks` (comma-separated, names as on screen) runs only
those.

### usb_tx_test (`make sd SD_APPS=usb_tx_test`)

A phone app sends its screen (up to 5 pictures a second by default) over USB; the MMI
shows it and logs everything. Usage, why text messages were dropped and
the car results: [src/usb_tx_test/README.md](src/usb_tx_test/README.md).

```
phone: android/ app  --USB (AOA bulk)-->  MMI: usb_tx_test  -->  screen + usb_tx_test.log
```

**How it works.** The MMI's USB host stack is QNX `io-usb` (EHCI + OHCI,
started by srv-starter) with the client library `libusbdi.so.2` on
efs-system (`/mnt/efs-system/lib`, symlinked to `/lib`). `usb_tx_test`
connects to it with a wildcard filter and does the
[Android Open Accessory](https://source.android.com/docs/core/interaction/accessories/aoa)
handshake with any new device (vendor requests 51/52/53). An Android phone
re-enumerates as `18d1:2d00`/`2d01` with a bulk IN/OUT interface (`ff/ff`).
That needs no root and no USB debugging on the phone. Android starts the
sender app because its `accessory_filter.xml` matches manufacturer
`mmi3g` / model `usb_tx_test`.

Protocol (`src/usb_tx_test/proto.h` = `android/.../Proto.kt`): frames of
`type u8 | len u16 LE | payload`, max 2048 B. `HELLO`/`HELLO_REPLY` say
who is at each end. `0x02`/`0x03` were `TEXT`/`ACK` (text messages,
retired 2026-10-25; not to be reused).

**Screen streaming.** Deliberately simple: one JPEG per picture, at most
*Max fps* a second (a slider in the phone app, 1–15, default 5), at
most two pictures in flight.

- The MMI announces what it can show in `VIDEO_CAPS` (sent with its
  `HELLO` and `HELLO_REPLY`): a landscape box (800×480, the whole
  screen) and a portrait box (400×480, the left half), plus 512 KB max
  per JPEG. A phone app streams only to an MMI that sent it.
- The phone (`CaptureService`, a MediaProjection foreground service)
  mirrors its screen into a virtual display sized to fit the box for its
  current orientation. It resizes the display when the phone rotates.
  When fewer than two pictures are in flight and 1/*Max fps* has passed
  since the last one was sent, it takes the newest image, compresses it
  (JPEG q60) and
  sends `VIDEO_START` (id, w, h, format, size) plus `VIDEO_DATA` chunks
  (id + ≤2046 B). A static screen produces no new images and so sends
  nothing. After a replug, the new link gets the last picture again.
- The MMI reassembles the picture (`vasm.c`), decodes it on its main
  thread (`video.c`, stb_image), draws it and acks it with `VIDEO_ACK`
  (id, status shown/bad/dropped, decode ms). A picture stays in flight
  until its ack (or 3 s). With two in flight, the next picture is already
  waiting on the MMI when the current one is on screen, so the MMI never
  waits for the phone's capture and transfer. It keeps only the newest
  waiting picture, so a slow MMI lowers the rate rather than queueing.
  `VIDEO_STOP` (or the link going down) brings
  back the status screen.
- `VIDEO_DETAILS` (phone, `show u8`) switches the status bar over a
  landscape picture on or off. The phone sends it on every connection
  and whenever *Details on the MMI* is flipped. The MMI starts with it
  off, so an older phone app gets the bare picture.

**Portrait vs landscape.** The picture keeps the phone's shape. It is
never rotated or stretched, only scaled down.

- **Landscape** (w ≥ h): centred on the whole screen. A 1080×2340 phone
  gives 800×368 (its 19.5:9 is wider than the MMI's 5:3, so 368 rows is
  the most it can fill without cropping). With *Details on the MMI* on,
  a 28 px status bar (state, video fps, decode and screen-copy times) is
  drawn over the top. For a 19.5:9 phone it sits in the black band above
  the picture.
- **Portrait**: full height on the left (a 1080×2340 phone gives
  220×480), with the status panel (state, video stats, footer) in the
  remaining ~580 px.

**Cost on the MMI.** Each picture means a JPEG decode, a redraw of the
800×480 screen in RAM and a copy into the VFB. CPU writes into the VFB
run at ~9 MB/s, so a full copy takes ~160 ms (docs/gpu-results.md). On
28.10 that copy was ~60 % of the MMI's time per picture. So `screen.c`
keeps a RAM copy of what each of the two VFB surfaces holds and writes
only the changed stretch of each row (`copy=changed`, the default; every
30 s, and for a new surface, a full copy). `usb_tx_test <sd> copy=full`
(the arguments field in `sd/apps.conf`) brings back the full copy, to
compare. While pictures arrive, the 1 s clock redraw is skipped.

**Performance metrics** (`perf.c`). Logging is expensive here: every log
line is fsynced to the SD card. So the threads that move pictures only
put a record into a RAM ring, and a lower-priority writer thread turns
the records into:

- one `perf: window` line in `usb_tx_test.log` per 10 s of streaming,
  plus a `perf: stream` line with the whole stream's totals when it
  stops. Each has pictures shown, fps, dropped and bad. It has avg/max in
  ms of every stage: `rx` (USB, `VIDEO_START` to the last `VIDEO_DATA`),
  `wait` (complete to picked up), `decode`, `render` (RAM canvas), `copy`
  (into the VFB), `flip` (layer manager) and `latency` (`VIDEO_START` in
  to on screen). It also has JPEG and VFB KB per picture, the main
  loop's busy % and the process CPU %.
- `usb_tx_perf.csv`, one line per picture with the same fields in µs
  (`t_ms,id,status,w,h,bytes,rx_us,wait_us,decode_us,render_us,copy_us,flip_us,vfb_bytes,vfb_rows,vfb_full,latency_us`),
  written 128 lines at a time (or every 30 s, or when the stream stops)
  with one fsync per batch.

The first three pictures of a stream are still logged one by one. The
log's start says the clock resolution (`perf: clock resolution`): with
a 1 ms tick, single µs values are only good to ±1 ms, while averages are
fine.

**Robustness.** Callbacks from io-usb only queue events; a manager thread
does all the work. Unplugging the phone, a transfer error, or a device
that isn't Android (USB stick, iPod) never ends the app: it goes back to
"Waiting for phone". A link that breaks while the phone stays plugged in
is reopened on the same device, up to 5 tries (see below for why that
happens). It retries `usbd_connect` every 2 s if io-usb isn't there. It
stops only on SD removal or SIGTERM/INT/HUP (no watchdog), then gives the
screen back to the HMI.

**What the unit's USB stack does (verified in Ghidra).** Checked against
`libusbdi.so.2`, `io-usb`, `vdev-medialaunch` and `iofs-pfs.so` (rev 51).
Full write-up, with the client-side consequences, in
[`docs/usb-stack.md`](docs/usb-stack.md):

| Question | Answer from the binaries | Where |
|---|---|---|
| Can another client lock us out of the phone? | No. `udi_attach` only counts openings; it refuses nothing but hubs. | io-usb `0x08046040` |
| Does the media stack's MTP driver take the accessory? | No. `ptp_parse_descriptors` needs bulk IN + bulk OUT + interrupt; the accessory interface (`ff/ff`) has no interrupt endpoint. | iofs-pfs `0x27940` |
| Do unplug / abort / detach complete our URBs? | Yes, all three complete them with `ABORTED` and send the completion event **before** the removal event. | io-usb `udi_in_out 0x08046620`, `0x08044e80`, `udi_transfer_complete 0x08044d40` |
| Can the link break while the phone stays plugged? | Yes. A client's `usbd_detach` aborts **every** URB on the interface's endpoints, other clients' included, and the media launcher attaches/probes/detaches new devices. Its MTP probe also resets a device whose `GET_DESCRIPTOR(0xEE)` times out (a STALL is fine). Hence the reopen. | io-usb `udi_detach 0x08046400`; medialaunch `0x080539c0`, `0x080537e0` |
| Removal while our URBs are pending? | libusbdi defers our `removal()` until they complete; no deadlock. | libusbdi `usbdi_event_thread 0x17a20`, `usbdi_synchronise` |
| Close/detach races | `usbd_close_pipe` / `usbd_detach` return `EBUSY` (and free nothing) while libusbdi still counts I/O, which it decrements only **after** our callback returns. A device never detached keeps io-usb's port in "holdoff" even after an unplug. So both are retried until the counters drain. | libusbdi `0x15aa0`, `0x15820`; io-usb `CLASS_ExtractDevice 0x08048e60` |
| Synchronous vs asynchronous `usbd_io` | Decided by the endpoint: EP0 waits for the reply (our control transfers), others are asynchronous and complete on the event thread. | libusbdi `usbd_io 0x19360` |
| Event thread | One thread (`usbdi_event_handler`, 16 KB stack) runs insertion, removal and completion callbacks in order. | libusbdi `0x17ca0` |

(Addresses as Ghidra loads them: libusbdi and iofs-pfs rebased at 0x10000.
`io-usb` from the efs extraction has a corrupt section header table; load a
copy with `e_shoff`/`e_shnum` zeroed.)

**USBDI header.** openqnx has no USB DDK headers, so `sdk/include/sys/usbdi.h`
is reconstructed from the QNX 6.5.0 SDK header (same ABI generation).
Checked against the unit's `libusbdi.so.2` disassembly: the
`usbd_connect_parm_t` offsets, the accepted `USBD_VERSION` (0 or `0x0101`),
the `usbd_setup_vendor`/`usbd_setup_bulk` argument order, and that buffers
must come from `usbd_alloc()` (the library calls `usbd_mphys` on them). The
usage pattern follows smartdevicelink's QNX USB transport, which shipped on
QNX head units.

**Build and run.**

```sh
make sdk                         # once, adds the libusbdi stub + host gcc
make test                        # C unit tests: protocol, pictures, JPEG, UTF-8, wrap, drawing
make sd SD_APPS=usb_tx_test      # receiver only (the default run has it last)
make apk                         # android/out/usb_tx_test.apk  (= make -C android)
make apk-test                    # Kotlin unit tests (protocol, picture fit)
make apk-install                 # adb install -r: adb runs in the container
make aoa-host                    # desk test: this PC plays the MMI; the newest
                                 # streamed picture lands in build/aoa-host/latest.jpg
```

Everything builds and runs in Docker: `mmi-sdk` for the MMI side,
`mmi-android` (`android/Makefile`, `android/docker/Dockerfile`) for the
phone app, `mmi-aoa-host` for the desk test. For `apk-install` and
`aoa-host`, stop a host adb server first (`adb kill-server`), since only
one program can own the phone's USB interface. The APK is signed with a
fixed key in `android/keystore/`, so reinstalls keep working after `make
clean`.

**In the car.**
1. Install the APK on the phone. Insert the SD card; the screen says
   *Waiting for phone*.
2. Plug the phone into the car's USB port (the AMI/USB socket).
   - The status bar goes amber (*switching*), then blue (*phone connected,
     app not open*).
   - The phone asks to open usb_tx_test for this accessory. Accept, and
     the bar turns green: *Connected*, with the phone model.
3. *Stream screen* (enabled once the MMI has sent `VIDEO_CAPS`): allow
   the screen capture, then use the phone as usual. Back, Home and
   turning the phone keep the app and the stream running. The
   notification has *Stop*. The app shows sent/shown pictures, the MMI's
   decode time and the round trip. *Details on the MMI* shows the MMI's
   status bar over a landscape picture (off by default).
4. Unplug and replug at will. Streaming carries on after a replug. Pull
   the SD card to end it.

Everything is in `usb_tx_test.log` on the card: every USBDI return code, the
device and interface descriptors, every state change and every frame. A
failed handshake or a removal also saves the system log to
`var/usb_slog_<n>.txt`. run.sh can't do that for this app, because pulling
the card ends it.

**Verified on the car (2026-10-21):** the whole USB path end to end.
Redmi Note 8 Pro in MTP mode, three texts received and shown
([`docs/usb-stack.md`](docs/usb-stack.md), "21.10 run"). On the first
connection the phone needs two AOA `START`s before it comes back as an
accessory. An accessory session nobody opens on the phone drops after
~18 s, and the app reconnects by itself. Both are handled. The 12.10–19.10
runs failed on `DEV_NOANSWER` for EP0 control requests with the same phone.
If that comes back, see the open questions in usb-stack.md.

**2026-10-25:** the link came up again, but no picture was streamed: each
turn of the phone recreated the app's activity, and the old one's late
`onClosed` cleared the link the capture service sends on. Fixed in the
phone app, not yet run on the car ([src/usb_tx_test/README.md](src/usb_tx_test/README.md)).

### hello6 (`make sd SD_APPS=hello6`)

Tests T1–T3 of [docs/phone-mirroring.md](docs/phone-mirroring.md) §7.
The default run is phase `v` alone, about 8 minutes. Park the car; text
screens say what to do.

| Phase | What | Output |
|---|---|---|
| `s` system info | `pidin`, mounts, `/dev/io-media`, `/dev/snd`, efs tool locations; the unit's `ShowVideoMemory`, `memusage`, `hbhogs` (not `ShowDisplayables`: it sets the shown list, and `ShowDisplayables 0` blanked the screen) | `hello6/sysinfo_before.txt`, `sysinfo_after.txt` |
| `c` CPU | memcpy, RGB565→BGRA, LZ4 on UI/photo/random 800×480 frames | `RESULT` lines |
| `g` GPU | CPU stores/memcpy to and from the VFB; `glTexSubImage2D` RGB565/RGBA/BGRA and DXT1 upload MB/s; visible upload+draw fps | `RESULT` lines |
| `v` video | 10 clips of 30 s (`media/mkclips.sh`) played by the unit's `testapp-file` with `-o mixer:DSPOUT_*` (its default PCM output cannot be attached on this unit), each shown on a video displayable (cid 41) two ways, 10 s each, with capture enabled through `/dev/videoctrl`: first VCAP 1 (flags `0x201`, VP 2), then VCAP 0 input 1 (`0x101`, VP 1 source 1, the DSP's video port; the way clips 1–3, 5 and 6 reach the screen, 2026-10-19/21). The driver turns the knob when the picture is visible. Clips 9–10 are written to the card while they play. | `hello6/play_<k>.log`, `hogs_<k>.txt`, `RESULT video_<k>` lines (incl. `output=`) |

`hello6 <sd> [phases]` picks the phases, any of `scgv` (default, and what
`apps.conf` runs: `v`).
The summary is shown on screen at the end and written to `hello6.log`.

**The unit's own tools.** hello6 runs `testapp-file` and the diagnostic
tools from the unit's efs (`/mnt/efs-system/bin`, served decompressed by
`inflator`). As a fallback, `make sd` copies them from
`dev/HN+R_EU_AU_P1001/extracted/MU9411/efs-system/51/bin/`. They are this
unit's files, inflated with `tools/qnx_inflator.c` where efs stores them
compressed. `make sd` skips any that are missing. They are started with
`spawnvp(P_NOWAIT)` with output to a file: QNX 6.3 refuses `fork()` in a
multithreaded process.

**Build.**

```sh
make clips                       # build/clips/ (also done by make sd SD_APPS=hello6)
make test                        # includes test_lz4: lz4mini vs the reference liblz4
make sd SD_APPS=hello6           # hello6 alone (~55 MB of clips); or add it to the list
```

`make sdk` again after pulling this: the SDK image gained `liblz4-dev`
for the LZ4 test.

**On the car (2026-10-21, phase `v`):** the efs tool paths, `spawnvp` and
video displayables all work. Five of the ten clips play on screen through
VCAP 0 input 1: MPEG-2 720×576, MPEG-4 640×480, H.264 Baseline 720×480,
and H.264 720×336 with and without audio. VCAP 1 stayed black for every
clip. The 800-wide H.264 clip, fragmented MP4, H.264 in MPEG-PS and both
growing files never start (io-media error `0x80002`). Details:
[docs/phone-mirroring.md](docs/phone-mirroring.md) §7.5.

## Limits / not done yet

- `math.h` is missing: openqnx doesn't include libm. hello3/hello4 declare
  `sinf`/`cosf` themselves and link the device's `libm.so.2` stub.
  `shle-nto-gcc` passes `-fno-builtin-sin*`/`-fno-builtin-cos*`. Without
  them, GCC fuses `sinf` + `cosf` of the same angle into glibc's
  `sincosf`, which QNX's libm doesn't have.
- **C++ without iostreams.** The car has `libstdc++.so.5` (GCC 3.3 ABI)
  and Dinkumware `libcpp.so.4`, neither compatible with modern GCC, so
  C++ apps carry GCC 16's libstdc++ (~180 KB). Its locale code needs glibc:
  no iostreams, `<sstream>`, `<format>`; no `thread_local`, futures or
  semaphores ([`sdk/include-c++/README.md`](sdk/include-c++/README.md)).
  Not run on the car yet.
- Floating point uses the same `-m4` double ABI as QNX. hello2's
  `glRotatef`/`glOrthof` calls will be the first real test of passing
  floats across the library boundary.
- **GCC 16 required for `-O1`+.** Debian's sh4 GCC 12.2 drops the second
  test of `a == 0 && b == 0` at `-O1` and up. That alone made hello2–4's
  register look failed on the first car run, although the layer manager had
  accepted it. GCC 16.2 compiles it correctly, so the build is `-O2` again;
  the `-O2` GCC 16 binaries haven't run on the car yet. Details:
  [`docs/gcc-sh4-treg-bug.md`](docs/gcc-sh4-treg-bug.md).
- `gf` and layer-manager details beyond what qnx-audi used are still
  unknown: register flags `2` (bit 0 selects a different displayable
  class), and the `{cid, 16, 19}` show list on exit.
- `/dev/ipc/ch8` is read and logged but never stops an app, so a stray press
  cannot cut a test short. Bytes 6 and 7 are confirmed to be main-rotary
  detents, one per direction; the rest of the format is still being decoded.
  See [`docs/platform-notes.md`](docs/platform-notes.md).

# rivit_app — native C apps for my MMI 3G+

Cross-compile plain C for the MMI 3G+ (QNX 6.3.2, Renesas SH7785 / SH-4A,
little endian) with Debian's stock SH4 GCC, packaged as a Docker image.

| App | What it does |
|---|---|
| `hello` | On SD insert, writes `hello.txt` (`hello world`) to the card. Toolchain smoke test. |
| `hello2` | OpenGL ES test. Logs full EGL/GL/gf diagnostics, runs a 20 s animation, and saves screenshots to the card. |
| `hello3` | GPU canvas: random-colour pixel blocks and a plasma effect, both rendered by the GPU from textures uploaded once, then a fill-rate benchmark. On-screen FPS counter. |
| `hello4` | 3D pipeline test: automatic pixel-readback probes (depth, culling, perspective, lighting, texture, fog, blend, stencil) shown as PASS/FAIL squares, then lit/textured/fogged 3D scenes and a triangle-rate benchmark. |
| `hello5` | Audio test: beeps on every turn of the main rotary knob. Plays a bundled PCM clip (`beep.wav`) through QSA (`libasound`) on the `tones1` device. First user of `/dev/ipc/ch8` input and of sound. |

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
`start_mode`/`frags_min` sit in the gaps and are inferred. `hello5` still logs
every `snd_*` return code and the negotiated format, so the first car run is
the final check — watch the `params`/`setup` lines in `hello5.log`.

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
2. **Debian ships a usable SH4 compiler:** `gcc-sh4-linux-gnu` 12.2 and
   binutils 2.40 in bookworm.
3. **The headers support modern GCC.** `sys/compiler_gnu.h` uses
   `__builtin_va_*` for GCC ≥ 3. GCC only needs QNX's macros:
   `-D__QNX__ -D__QNXNTO__ -D__SH__ -D__LITTLEENDIAN__`, with the Linux
   macros undefined.
4. **The device libc exports GCC's SH helpers itself:** `__set_fpscr`,
   `__fpscr_values` and `__sdivsi3_i4`.
5. **FPU mode:** `crt1.S` calls `__set_fpscr(0xc0400)`, which sets PR=1
   (double precision), DN=1, and enables the divide-by-zero trap. That's
   exactly GCC's `-m4`. GCC 12 switches FPU modes inline and doesn't need
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
10. **The SD autorun chain** is in the toolkit already:
    1. `proc_scriptlauncher` finds `copie_scr.sh` (XOR-encoded by
       `builder/encoder.py`).
    2. That runs `core/copie_scr_plain.sh`.
    3. That runs `exec ksh ./run.sh $SDPATH`.
    4. `run.sh` copies `bin/*` to `/tmp/rivit_app/` and runs the test apps
       one after another, logging to `var/rivit_app.log` (see
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
  | `0x80040501` | unknown |

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
Dockerfile            mmi-sdk image: debian bookworm + gcc-sh4-linux-gnu + sysroot
Makefile              host entry point (sdk / all / check / sd / shell / clean)
sdk/
  shle-nto-gcc        compiler wrapper: QNX headers, crt objects, link flags
  build-sysroot.sh    runs in `docker build`: headers, crt, stubs, 6.3.2 patch
  mkstubs.py          stripped device .so → link-time stub (.S)
  mkscreens.py        pre-renders run.sh's status screens (PNG) for `make sd`
  fonts/              Terminus Font 8x16 / 16x32 console fonts (SIL OFL, OFL.txt)
  crtarray.c          _preinit_array/_init_array/_fini_array for crt1.o
  include/gf/         reconstructed gf.h / gf3d.h (QNX 6.3.2 layouts)
src/
  common/             shared by graphics apps
    log.c             SD log, fsync per line
    lmgr.c            /dev/layermanager client (devctl + text commands)
    gfx.c             gf + EGL bring-up / teardown (port of qnx-audi Screen)
    bmp.c             24-bit BMP writer
    quit.c            clean-stop sources: timer, button, signals, SD removal, watchdog
    hud.c             seven-segment FPS counter + rectangles
  hello/hello.c
  hello2/main.c
  hello3/main.c       scenes, timing, fill-rate table
  hello4/main.c       3D scenes, timing, triangle-rate table
  hello4/mesh.c       cube / torus / ground grid (indexed, client-side arrays)
  hello4/probe.c      glReadPixels pass/fail checks of the 3D pipeline
sd/run.sh             launched by copie_scr.sh on SD insert: runs the test apps
                      (@APPS@ and screen times filled by make)
sd/apps.conf          per-app timeout, arguments, description (run.sh + screens)
sd/README.md          short user guide, copied to the card as README.md
build/                output (gitignored); build/sd/ = SD card root
```

Use the wrapper like gcc in your own builds:

```sh
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src mmi-sdk \
    shle-nto-gcc -O2 -o app app.c -lGLES_CM -lgf
```

## Testing on the car

General procedure:
1. FAT32 card. Copy `build/sd/*` to the root.
2. Turn the ignition on and wait for the MMI to boot fully. Insert the card.
3. A start screen comes first. Then each app gets a "Next" screen (what it
   does and what you should see) and a result screen. Wait until the
   "all done" screen has gone (about 4 min for all five apps), then pull
   the card.

If neither the apps' output nor `var/rivit_app.log` appears,
`copie_scr.sh` didn't run.

### The test run

`run.sh` (ksh, no native runner) does this for each app in `SD_APPS`:

0. **Screen "rivit_app"** (`start.png`) for 5 s, once: what the run is,
   and don't pull the card before "all done".
1. **Screen "Next: <app>"** for 5 s: its title and what you should see or
   do, its arguments and timeout, and the run order. This is the pause
   before the app starts.
2. **The app**, with stdout and stderr in `var/<app>_out.log`. Three `=== `
   lines are appended at the end: exit code, run time and verdict. A watcher
   sends TERM when the app runs over its timeout, and KILL 15 s later.
   Then `sloginfo` goes to `var/<app>_slog.txt` and core files are moved to
   `var/` (see "When an app crashes").
3. **Screen "<app>: <verdict>"** for 8 s, with what the verdict means and
   where its logs are.

Then a final screen "k of n apps OK" for 15 s. The verdicts, with exit
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
hello5 runs 30 s there, not its 600 s default. The screen times are
`PRE_S`/`POST_S`/`FINAL_S` in the Makefile.

**Screens.** They are shown with the toolkit's `core/bin/showScreen`
(DrGER's tool, already proven on the car), as
`showScreen -t <seconds> <png>`. `-t` shows the image and blocks until
the time is up. showScreen can only show a PNG, so `sdk/mkscreens.py`
pre-renders every screen a run can need into `screens/` on the card
(42 PNGs for five apps, about 300 KB):

| File | Shown |
|---|---|
| `start.png` | once, first |
| `next_<app>.png` | before the app |
| `<app>_<VERDICT>.png` | after it, one per verdict |
| `done_<k>.png` | at the end: k of the n apps OK |

To preview them, look at `build/sd/screens/` after `make sd`. If
showScreen or a PNG is missing, or showScreen returns early, `run.sh`
sleeps the rest of the time, so the timing doesn't change. showScreen's
own output goes to `var/showscreen.log`.

**Not yet verified on the car:** that the screen goes back to the HMI
when `showScreen -t` ends (the toolkit's scripts only ever ran it without
options, in the background). Also not verified: that a hello app can take
the screen right after it. If the final screen stays up, the MMI reboot
key combo clears it.

### Stopping an app

`hello` writes its file and exits immediately. hello2–hello4 always
leave through the same teardown (`gfx_shutdown`), which hands the screen
back to the HMI. The log ends with `stop requested: <reason>` and
`exit (stop reason: …)`. The ways to stop them (`src/common/quit.c`):

| How | Notes |
|---|---|
| Wait | The normal end: 20 s for hello2, 3 × 10 s for hello3, 4 × 10 s for hello4. |
| **Press a hardware button** | Read from `/dev/ipc/ch8`, as qnx-audi does. Any message that isn't a turn of the rotary or volume knob counts as a button. The first 30 raw messages are logged as hex to help decode the key protocol. Not verified on P1001. The HMI may also react to the press. |
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
| `var/hello2_out.log` | stdout/stderr, exit code, verdict (`var/rivit_app.log`: `run.sh` itself). |

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
| 9 | `stencil` | Drawing only where a stencil mask was written. **SKIP** if the EGL config has no stencil bits, which is likely: it doesn't ask for any. |

Then four scenes of 10 s each (`hello4 <sd> <seconds>` changes the length):

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

## Limits / not done yet

- `math.h` is missing: openqnx doesn't include libm. hello3/hello4 declare
  `sinf`/`cosf` themselves and link the device's `libm.so.2` stub.
  `shle-nto-gcc` passes `-fno-builtin-sin*`/`-fno-builtin-cos*`. Without
  them, GCC fuses `sinf` + `cosf` of the same angle into glibc's
  `sincosf`, which QNX's libm doesn't have.
- **C only.** The car has `libstdc++.so.5` (GCC 3.3 ABI) and Dinkumware
  `libcpp.so.4`. C++ from GCC 12 isn't ABI-compatible with either.
- Floating point uses the same `-m4` double ABI as QNX. hello2's
  `glRotatef`/`glOrthof` calls will be the first real test of passing
  floats across the library boundary.
- `gf` and layer-manager details beyond what qnx-audi used are still
  unknown: register flags `2`, command `0x501`, and the `{cid, 16, 19}`
  show list on exit.
- Input is limited to "any button = stop" on `/dev/ipc/ch8`. The message
  format beyond qnx-audi's byte 4–7 knob counters is unknown. The hex log
  in `hello2.log`/`hello3.log` is the starting point for decoding it.

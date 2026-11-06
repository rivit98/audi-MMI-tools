# MMI 3G+ platform notes — what the car has actually told us

Confirmed facts about this unit, gathered from car runs of our test apps and
from disassembling the device's own libraries. Everything here has been
observed; anything still guessed is marked **[unverified]**. The point is
that nothing below needs re-testing — build on it.

GL measurements are written up in [`gpu-results.md`](gpu-results.md).
Disassembly was done with IDA/objdump on the firmware libraries in
`third_party/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/mnt/ifs-root/usr/lib/`.

---

## 1. Toolchain

- Debian sid `gcc-16-sh4-linux-gnu` (16.2.0) driven as a QNX 6.3 SH4
  compiler by `sdk/shle-nto-gcc` produces binaries the MMI loads and runs.
- **`-O2` is correct under GCC 16.** GCC 12.2 miscompiles `a == 0 && b == 0`:
  `lmprobe`'s on-car canary reads `MISCOMPILED` under GCC 12 and `correct`
  under GCC 16, on the same unit. See [`gcc-sh4-treg-bug.md`](gcc-sh4-treg-bug.md).
- The build-time canary (`sdk/canary.c`) stays, because the SDK image is
  `FROM debian:sid-slim` — a rolling base whose GCC 16.x can change under us.
- **Code is built for SH-4A with LL/SC atomics** (`-m4a
  -matomic-model=hard-llcs`). GCC's sh4 default, gUSA atomic sequences,
  loses updates under QNX, whose kernel doesn't restart them (hello8
  `atomics stress`: 9 % lost, LL/SC none). QNX's loader accepts the `sh4a`
  ELF flag. See [`cpp-apps.md`](cpp-apps.md#threads-and-atomics).
- ET_EXEC at base `0x08040000`, `--hash-style=sysv`, 4 KB segment alignment,
  interpreter `/usr/lib/ldqnx.so.2`. Crash addresses from the dumper map
  straight onto the unstripped `build/<app>` (`make addr`).

## 2. A divide by zero is fatal — by design

The single most surprising property of this platform.

QNX enables the SH4 FPU divide-by-zero trap for every thread, in two places:
`lib/c/startup/sh/crt1.S` calls `__set_fpscr(0xc0400)` as the very first
thing `_start` does, and `services/system/ker/sh/nano_fpu.c` sets the same
bits as the kernel default. The kernel comment explains why: QNX uses the FPU
to implement **integer** division, so the trap is how integer `/ 0` is
caught.

Consequences:

- `x / 0.0f` is `SIGFPE`, not `inf`. Integer `/ 0` and `% 0` too.
- Guard denominators. Do not rely on IEEE semantics anywhere.
- The GL driver's linear fog divides by `END - START` whenever fog state
  changes, so setting `GL_LINEAR` while START == END is a `SIGFPE`. Use
  `gfx_fog_linear` instead of `glFogf`. There are more unguarded divides
  waiting in both the GL driver and libasound.

Full writeup, including how to decode FPSCR from a core file and how to
disassemble a stripped firmware `.so` from a `mapaddr`:
[`qnx-sh-fpu-div0.md`](qnx-sh-fpu-div0.md).

## 3. Layer manager and the VFB — solved

The reverse-engineered `showScreen` sequence is correct and deterministic.
`lmprobe` runs it end to end with `rc=0` at every step, and its `d-shot` is
**byte-identical between runs** (`d7d051ec…`).

| | |
|---|---|
| version | `lm_check_version` → **1.1** (what qnx-audi expects) |
| sequence | unregister → register cid 40 flags 2 → `getVfb` → draw → `updateVfb` → `show {40,16,19}` |
| VFB | 800×480 `BGRA8888`, stride **3200** (= 800×4), `vaddr=3ebbf000`, `paddr=0xdeabc000` — same every run |
| `sid` | 1 |
| `uid` | `4294967295` |

Two things that look like errors and are not:

- `lm_unregister_displayable(40)` **before** registering returns `rc=22
  (EINVAL)`. Expected — nothing is registered yet. After the run it returns
  `rc=0`, confirming a clean lifecycle.
- `gf_layer_query` for one index past the last format returns
  `GF_ERR_IODISPLAY`. That is the enumeration terminator, not a failure.

**Do not hardcode `alpha_sid`.** It is allocated per run and has been
observed as 12, 16, 22, 24, 28 and 34.

**The VFB is double-buffered, and its `sid` changes on every `updateVfb`.**
Confirmed in layermanagerV2 (P1001, `CSurfacesManager`). Each displayable
owns two surfaces and a front index `cur`:

| call | what the server does | address |
|---|---|---|
| `getVfb` | marks `surface[!cur]` not ready and returns it: the **back** surface | `0x080508A4` |
| `updateVfb` | marks `surface[!cur]` ready, then `toggleSurfaces` flips `cur`: what you drew becomes the front, the old front becomes the next VFB | `0x080514EC`, `0x080508F0` |
| (displayable flag bit 3, `this[0xC] & 8`) | after the flip, blits the new front into the new back so the back starts out equal to the screen | `0x08051572` |

So a client must call `getVfb` again after **every** `updateVfb` and draw
into the sid it returns. A client that keeps drawing into the first sid only
gets its picture on every other update. The updates in between show the
other surface, which nobody ever wrote: pink/beige vertical-band noise over
the top ~60 % of the screen. A client that redraws periodically then
alternates between its UI and that noise. One-shot users (showScreen,
lmprobe) update once, which is why they never see it. The same never-written
surface is the "noise over the map" in the GL apps' `s-shot`s (§7).
`src/common/screen.c` re-fetches the VFB after each update and keeps both
surfaces attached.

Because this is settled, `lmprobe` is not in the default `TESTS` — it is
still built (`make sd SD_APPS=lmprobe` puts it on the card).

## 4. Display and layers

| | |
|---|---|
| device | `gf_dev_attach` → `ndisplays=6`, `description='nv'` |
| display | 800×480, `BGRA8888`, **refresh 60 Hz**, `nlayers=8`, `main_layer=0` |
| our layer | **7** (topmost) |
| layer 7 formats | `BGRA8888`, `PACK_ARGB1555`, `PACK_RGB565`, `PAL8` |
| layer caps | `0xdc`, `src_max=4096x4096`, `dst_max=800x480` |

Six displays are reported on a single-screen head unit; only display 0 is
ours to use.

## 5. The GPU: NV18, fixed function, and its hard limits

`GL_RENDERER: NVIDIA EMBEDDED Build 6.0180/NV18` — **NV18 is the GeForce4 MX
core, circa 2002**. That one string explains every limit below. None of them
will ever lift.

| | |
|---|---|
| `GL_VERSION` | `OpenGL ES-CM 1.0` — fixed function, **no shaders** |
| `EGL_VERSION` | 1.3, `EGL_VENDOR` NVIDIA, `EGL_CLIENT_APIS` `OpenGL_ES` |
| `GL_MAX_TEXTURE_SIZE` | 2048 |
| `GL_MAX_TEXTURE_UNITS` | **2** |
| `GL_MAX_LIGHTS` | 8 |
| `GL_MAX_CLIP_PLANES` | **0** — no user clip planes at all |
| modelview / projection stack | 32 / 10 |
| `GL_MAX_VIEWPORT_DIMS` | 4096×4096 |
| colour / depth | 8888, depth 24, subpixel bits 4 |
| compressed formats | 13 (S3TC / DXT1 present) |

Extensions present: `GL_OES_query_matrix`, `GL_OES_read_format`,
`GL_NV_fence`, `GL_EXT_texture_compression_s3tc`, `..._dxt1`,
`GL_EXT_texture_lod_bias`, `GL_NV_register_combiners`,
`GL_OES_compressed_paletted_texture`, `GL_NV_attrib_stack`, `GL_EXT_bgra`,
`GL_SGIS_texture_lod`, `GL_NV_texture_rectangle`, `GL_OES_texture_cube_map`,
`GL_NV_texgen_full`, `GL_OES_matrix_palette`, `GL_OES_extended_matrix_palette`,
`GL_OES_draw_texture`, `GL_EXT_separate_specular_color`, `GL_NV_residence`,
`GL_EXT_timer_query`, `GL_OES_EGL_image`, `GL_OES_vertex_buffer_object`,
`GL_NV_complex_primitives`.

Notably **absent: `GL_OES_framebuffer_object`** — no FBO render-to-texture.
`EGL_KHR_image` / `GL_OES_EGL_image` and pbuffers are the offscreen routes.

Three of these are worth remembering because they solve problems we have:

- **`GL_EXT_timer_query`** — real GPU time, immune to the vsync quantisation
  in §6.
- **`GL_NV_fence`** — sync without a full `glFinish`.
- **`GL_OES_vertex_buffer_object`** — the apps still use client-side arrays.

### EGL configs

30 configs. All report `visual=0(INVALID)` (no native window system) and
`caveat=0x3038` = `EGL_NONE`, i.e. **no config is slow or non-conformant**.
`surf_type=0x007` on the 8888 configs = WINDOW | PIXMAP | **PBUFFER**, so
offscreen pbuffer rendering is available.

**Stencil is available but must be asked for.** Configs 1, 5, 11, 15, 21 and
25 offer `s8`. `eglChooseConfig` treats an omitted `EGL_STENCIL_SIZE` as
"0 is acceptable" and returns the first match, which is stencil-less: the
app gets `GL_STENCIL_BITS: 0` and stencil tests do nothing. `gfx.c`
requests 8 bits and falls back if a layer format has no such config.

## 6. Everything is vsync-locked — read performance numbers carefully

`eglSwapBuffers` blocks on the 60 Hz refresh. Frame times therefore quantise
to multiples of 16.67 ms, and the per-second `worst frame` values across all
logs bear this out almost exactly: 17, 33/34, 50, 67/68, 83, 101, 117, 134.

hello3's fill-rate sweep is the clearest demonstration:

| layers | fps | vsyncs/frame | reported Mpixel/s |
|---|---|---|---|
| 1 | 58.1 | 1.03 | 22 |
| 2 | 46.0 | 1.30 | 35 |
| 4 | 29.0 | 2.07 | 44 |
| 8 | 19.7 | 3.05 | 60 |
| 16 | 14.8 | 4.05 | 91 |
| 32 | 9.7 | 6.19 | 120 |

Those fps values are 60/1, 60/2, 60/3, 60/4, 60/6. So **the table measures
vsync quantisation, not fill rate.** The apparent "fill rate" climbs with
load only because quantisation waste shrinks as frames get heavier.

Treat every Mpixel/s figure as a **lower bound**. The true fill rate is
somewhere above ~129 Mpixel/s and still unmeasured, because even 32 layers
is quantised. `GL_EXT_timer_query` is the way to measure it properly. The
frame-budget model built on these numbers is in
[`gpu-results.md`](gpu-results.md) §3.2.

Also: `swap failures=0` and `glError=0x0000` throughout, in every app. The
EGL/GL path itself is not a source of risk.

## 7. Screen capture: only `glReadPixels` is trustworthy

Layer-manager captures never contain GL output. GL draws on hardware layer 7
through the gf 3D target, outside the layer manager, which composites and
captures only its own displayables ([`gpu-results.md`](gpu-results.md) §2).

| path | result |
|---|---|
| `glReadPixels` + `bmp_write_rgba` | **correct** |
| `d-shot` of a **CPU-written** VFB (lmprobe) | **correct**, byte-identical across runs |
| `d-shot 40` of a GL app | **noise**: the never-written VFB of cid 40 (§3), not our frame |
| `s-shot 0` (whole display) | the **live HMI** (the clock on the map moves between shots), without our GL output. Shots taken in the same minute have identical md5s |

Use `glReadPixels` for anything GL-rendered. A pbuffer (see §5) is the
untried route to a clean offscreen capture. **[unverified]**

## 8. Input: `/dev/ipc/ch8`

The unit's only user input reaches us as 27-byte messages on `/dev/ipc/ch8`.
Each message is the current state of the controls: bytes 14–15 are normally
`01 02` (byte 17 = `01` too), and a message with all control bytes zero
follows a button release. Decoded from a fixed sequence of turns and presses
done by the user against the full 27-byte dumps:

| byte | value | control |
|---|---|---|
| 6 | detents | main rotary knob **clockwise (right)** |
| 7 | detents | main rotary knob **anticlockwise (left)** |
| 4 | detents | volume knob **up** |
| 5 | detents | volume knob **down** |
| 3 | `0x04` | **MENU**? The two buttons came in a different order in two decoding sessions, so the name is **unconfirmed**. The press always falls in the same second as the HMI's `OnOff new system state is 48` |
| 1 | `0x02` | **top-left soft key**? The other of the two: **unconfirmed** |
| 1 | `0x40` | another button (which one is unknown) |
| 1 | `0x80` | another button (unknown) |

```
00 00 00 00 00 00 01 00 00 00 00 00 00 00 01 02 00 01 ...  main knob 1 detent right
00 00 00 00 00 02 00 00 00 00 00 00 00 00 01 02 00 01 ...  volume 2 detents down (fast turn)
00 00 00 04 00 00 00 00 00 00 00 00 00 00 01 02 00 01 ...  MENU down
00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 02 00 01 ...  button released
```

- The knob bytes are **counts**: a quick turn sends one message with `02`.
  Main knob 5 + 5 clicks gave exactly 5 + 5 messages. Volume 3 up gave 3,
  and "3 down" gave 4 messages carrying 5 detents (`01 01 01 02`), so the
  knob reports what it physically moved.
- **This car's volume knob is slightly faulty** (the user). Its turns often
  come with detents in the other direction mixed in (`Incr=1` among
  `Incr=-1`s, `-6` in a slow turn up), in ch8 and in the MMI's own
  `Encoder Key=0x11` log alike. That is the knob, not the protocol. The
  main knob's counts are clean.
- MMI3GApplication logs the same turns in the system log, one line per ch8
  message at the same moments: `[KEY] Encoder Key=0x10 Incr=±n` for the
  main knob (byte 6 = +n, byte 7 = −n) and `Encoder Key=0x11` for the
  volume knob (byte 4 = +n, byte 5 = −n), sizes included (`02` →
  `Incr=-2`). Checked over several runs.
- Buttons are bitmasks in bytes 1 and 3, sent on press, then an all-zero
  message on release (~200–350 ms later).
- The HMI still acts on the knobs and buttons while our app is on screen
  (user's observation). Keeping input away from the HMI is open
  (`docs/TODO.md`). A MENU press brings the HMI's main menu up over our
  displayable, and it stays on top: the HMI re-shows its own screens when
  it handles a button, and our layer doesn't come back by itself.
- Bytes 16–26 are **state, not controls** [inferred]: byte 16 is constant
  within a run but has been seen as `00` in one and `03` in another,
  byte 26 as `11`, and bytes 23/25 change with no control touched. hello7
  does not decode them. There are also 29- and 33-byte messages with
  byte 14 = `02`, and unnamed button bits byte 1 `0x04`/`0x10`/`0x20` and
  byte 0 `0x01`/`0x02`/`0x08`/`0x10`/`0x40` (src/hello7/README.md).
- **Byte 15 looks like the unit's power state** [inferred]. It is `02` in
  normal operation. With nothing touched, ch8 sends a 29-byte message with
  bytes 14–15 = `02 02`, then `01 03` 0.2–0.6 s later and `01 00` about
  3 s after that. This sequence has been seen twice:
  - Once the run ended there: the app's log, `run.log` and the next app
    all stopped. At the first message there was a loud click, the radio
    stopped, and a tone fell from ~1 kHz to ~700 Hz over 2.5 s, like a
    motor running down. The car most likely switched off (ignition off or
    the unit going to standby), and the SD writes stopped with it (or the
    card was pulled).
  - Once the unit kept running: the app played on for 3 more minutes and
    run.sh logged its result. MMI3GApplication logged `OnOff new system
    state is 5` in the same second as the sequence, `7` 18 s earlier, and
    `19` with `AMFMTuner disconnected` 2 min later. The states seen while
    running normally are `14`, after `49 20 35 4 6` at boot.

  So byte 15 `03`/`00` is a power state on the way down, with the unit
  possibly still running for minutes [inferred]. If that holds, ch8 gives
  an app a few seconds' warning to save its state. To confirm: switch the
  ignition off on purpose during a run and compare.

**No key or button stops an app.** Runs end on their own timer, a signal, SD
removal or the watchdog — a stray press during a test cannot cut it short.
`quit.c` still reads ch8, logs the first messages raw and offers the bytes to
an app observer (`quit_init_ex`), which is how hello5 counts volume detents.
**hello7** is the free-form input tester: it names every control as it is
used (`src/hello7/keys.c`) and reports unknown bits by byte, so the unknown
`0x40`/`0x80` buttons can be named with one press each.

## 9. Audio — only io-media's `TDM1_20_SAD` is heard

Sound written to the unit's audio devices plays (it is consumed in real
time) on every path we tried, but only one path is actually heard in the
car: io-media's mixer output `DSPOUT_TDM1_20_SAD`, the same way the unit's
own `testapp-file` plays. The QSA (io-audio) devices are never heard.

### What plays and what is heard

Tested with the radio on, playing chimes and ping trains:

| path | plays | heard |
|---|---|---|
| QSA `snd_pcm_plugin_write`, card 0, devices 5, 1, 2, 3, 4, 0 (`tones1`, navi, SDS, BT-Tr, NAD, ent3) | yes: every write consumed in real time (write + `EBUSY` ≈ 600–650 ms for a 600 ms clip), `hb_fpga: Play buffer underrun strmN` as each stream runs dry, `Audio Buffer Failed to Drain` at each close | **no** |
| io-media `mixer:DSPOUT_TDM1_20_SAD` | yes, to `INPUT_EOF` | **yes, clearly**; replaces the radio while it plays |
| io-media `mixer:DSPOUT_TDM1_51` | yes | no; **the car audio goes silent** while it plays |
| io-media `mixer:DSPOUT_TDM1_20` | yes | no; **the radio stopped** for the length of the chime |
| io-media `mixer:DSPOUT_TDM2_20`, `TDM2_20_SAD`, `TDM3_20`, `TDM3_20_SAD` | yes | no |
| io-media `mixer:DSPOUT_TDM1_51_SAD` | doesn't attach while the MME plays: "Mixer entry for DSPOUT_TDM1_51_SAD: Already in use" | — |

`testapp-file` exits after 640–700 ms for a 600 ms clip and 2.46–2.57 s
for a 2.5 s clip. The system log has nothing from io-media or the audio
stack around any of these.

io-audio → FPGA works, so the QSA block is downstream. io-audio's `hb_fpga`
mixer has only PCM and I/O elements, no volume or mute, so the gating is not
in io-audio. MMI3GApplication's `CHBGWavePlayer` plays the system tones on
`/dev/snd/tones1` too, but the binary also names audio sources such as
`eSRC_SYSANN_BEEP`, `eAmxRingerToneHFP`, `CAmpLC` and `LC_SRCSYSANN3_BEEP`.
Most likely the amplifier (over MOST) only mixes a stream in once
MMI3GApplication's audio management has connected that source.
**[inferred]**

io-media-nvidia's mixer (efs-system, strings at `0x237ed4`) runs on the
DM642 and knows the outputs `DSPOUT_TDM1_51(_SAD)`, `TDM1_20(_SAD)`,
`TDM2_20(_SAD)`, `TDM3_20(_SAD)` and `PCI1..3`; the inputs
`DSPIN_TDM1..3_20`; and four player zones `NV_AUDIO_MIXER_IN_ZONE_1..4`.
Each output takes one player ("mixer entry"), and the DSP mixer can stamp a
"SAD" header on a stream (`AMixerSadHeaderSet`). The MME plays music into
`mixer:DSPOUT_TDM1_51_SAD?delay=130` (its `defaultoutput`), and
MMI3GApplication builds mixer graphs from `mixer:DSPIN_TDM1_20` (presumably
the tuner) to the `DSPOUT_TDM1` outputs.

Reading [inferred]: TDM1 carries the main entertainment stream to the
external MOST amplifier, which decodes it with its SAD header. 2.0 with the
header (`TDM1_20_SAD`) is a format it accepts, so it is played. `TDM1_20`
and `TDM1_51` without the header drive the same TDM port in a format the
amplifier doesn't take, so the stream it was decoding is gone (silence) and
ours isn't played either. TDM2/TDM3 and the io-audio devices reach the
amplifier on channels it only mixes in when MMI3GApplication's audio
management switches that source on.

### How `TDM1_20_SAD` behaves next to the car's own audio

Measured from recordings of 880 Hz ping trains (a 0.5 s ping every 2 s), with
details in [`src/hello5/README.md`](../src/hello5/README.md):

- **It replaces the radio, it doesn't mix.** For the whole of the file,
  everything between the pings is at the recording's noise floor (15–25 dB
  below the radio). The radio comes back ~0.2 s after the file ends. (Once,
  the radio's bass faded back in under the pings from ~10 s on; that did
  not repeat and is unexplained.)
- **It also attaches while the MME plays Bluetooth music, and mutes it.**
  AVRCP reports `PLAYING` throughout, but no music is heard until
  `testapp-file` ends.
- **Its level is the car's volume.** The pings follow the volume knob:
  ~21 dB down after a run of detents down, ~12 dB back up after detents up.
  The knob is too noisy to give a dB-per-detent figure (§8). With the
  volume untouched the pings sit ~15 dB over the cabin noise.
- **The graph's volume `devctl` has no effect.** At volume 0 the pings are
  as loud as at 100 (±0.3–0.5 dB); volume 30 likewise. Why is below.
- io-media plays a file an app wrote to `/tmp` (RAM).
- **An app's own io-media client plays sound** (hello5's `iom.c`, no
  `testapp-file`): STATUS playing +24 ms after play, first TIME +480 ms.
- **Queued files are not gapless:** chaining files by `INPUT_EOF` →
  attach the next adds **~350 ms of silence** per file (2.33–2.39 s
  instead of 2.0). io-media sends **one `FINISHED` per queue**, not per
  file.

What this means for an app with sound: it can play through
`TDM1_20_SAD`, and the user's radio or music is muted while it does
(confirmed for the radio and for Bluetooth media). Its level is the car's
volume, set with the volume knob. The app's own graph volume does nothing
(below, confirmed in code). That suits short prompts and a full-screen
media app, but not background sound over the radio. To be quieter than
the car volume, an app has to scale the samples it writes.

### Why the graph volume does nothing

io-media-nvidia, P1001 efs-system, IDA. The `mixer:` output plugin's ops
table is at `0x8279360`. Its fourth entry, `0x8050570`, is
`mixer_set_volume` (its own log strings). It takes only the first byte of
the 4-byte volume payload, so balance and fade are ignored. It computes
`gain = volume / 100.0` and calls virtual slot `+0x0C` of the DSP
`AudioMixer` singleton (`sub_80A1940`, object `0x83636C4`, vtable
`0x828813C`). That slot (`0x80A3D20`) is a stub:
`mov.l #0x8000000C, r0; rts`. Nothing reaches the DSP. The client still
gets `EOK` from the devctl, so the generic graph code doesn't pass this
error back [inferred].

The real DSP gain command does exist. Vtable slot `+0x20` (`0x80A2720`)
sends `AMixer` command 2 (`GainSet`; the constructor registers commands
0–10 by name, `0x80A1C00`) for pins 1–6. Its only wrapper is
`sub_804E13C(pin, gain)`, and nothing in the binary references it (no
call, no pointer). So no client of io-media can set a DSP mixer gain.

**`volume_ctrl` isn't a volume.** `FUN_08055858` parses the mixer URL's
query: `volume_ctrl=yes` sets a flag, and `mime`/`mimetype`, `delay`,
`EncodingBitrate` and `PriorityAdjust` fill the other fields. The only
reader of the flag is the encoder set-up (`FUN_080550ae`, mix-out to
`DSPOUT_PCI1..3`): `yes` makes the encoder shareable ("Volume_ctrl=yes so
Encoder is shareable"), `no` makes it private. It means nothing for
playback.

### io-media from our own code

From `testapp-file`'s disassembly, P1001 efs-system. `testapp-file` links
no io-media library (NEEDED is only `libc.so.2`). It creates a graph by
opening `/dev/io-media/graphs/trackplayer/<name>`
(`O_RDWR|O_CREAT|O_EXCL`, 0626) and drives it with `devctl`s: attach the
output URL, set the volume (0–100, plus balance and fade), attach an input
(`file:`, `tmpfile:` or a plain path; the extension picks the media type,
`.wav` = 3), play, and read events from the same fd. Closing the fd
destroys the graph. The codes and payloads are in
[`src/common/iom.h`](../src/common/iom.h). **[confirmed in code and on the
car: hello5's own client plays sound this way]**

- `testapp-file` always exits with **status 1**. `main` has one epilogue
  (`mov #1,r0` at `0x80412A4`), and every path, success included, ends
  there. Read its output instead: success is `FINISHED … 0 left to do`
  followed by `Done, destroying the graph`.
- Its stdout is **fully buffered** when it is a file: no `setvbuf` or
  `fflush`, and QNX's `BUFSIZ` is 1 KB. So its log lines carry no timing,
  and stderr lines can come before stdout lines printed earlier. Log every
  event yourself, with the time since play.
- Its multi-URL mode chains files: on `INPUT_EOF` (the file has been read,
  which comes early: before 0.4 s of a 2.5 s clip) it detaches the input
  and attaches the next. On `FINISHED`, if files are left, it plays again.
  This is the chaining that adds ~350 ms per file (above).
- The mixer URL's query keys are `delay`, `volume_ctrl`, `mime`/`mimetype`,
  `EncodingBitrate` and `PriorityAdjust`. There is no `volume=`.
- `_SAD` is the same TDM port with flag `0x40000` in io-media's output
  table at `0x82A8488` (`TDM1_20_SAD` `0x40002`, `TDM1_20` `0x2`). It
  presumably switches on the DSP's `AMixerSadHeaderSet`, the header the
  amplifier wants **[inferred]**.

### QSA (libasound) details

`snd_pcm_open` on card 0, device 5 (`tones1`, the UI-tone mixer input)
works, `snd_pcm_plugin_params` accepts 44.1 kHz mono S16_LE, and full
52,920-byte `snd_pcm_plugin_write`s go through — it is just never heard
(above).

- `snd_pcm_plugin_prepare` returns **`EBUSY` while the previous clip is still
  playing** (within the clip's length after a successful write). That is
  not a failure; count it separately.
- The setup read-back (`snd_pcm_channel_setup_t`) has **no `sync[]` field**:
  `snd_pcm_plugin_setup` (libasound `0xC378`) memcpys the 17-byte format block
  to **`@0x08`**, not `@0x18` like params. Reading it at `0x18` gives
  "negotiated 0 Hz, 0 ch". The header is fixed and asserted.
- The enum values must come from io-audio (`0x0804E780`). Values copied
  from ALSA 0.5 fail with `why_failed=2` = `BAD_START`.

There is no QSA header anywhere in the openqnx source we build from, so
`sdk/include/sys/asoundlib.h` is reconstructed from the device's own
`libasound.so.2`. Offsets confirmed by disassembly:

| offset | field | how it was confirmed |
|---|---|---|
| `0x00` | `channel` | validated 0..1 |
| `0x04` | `mode` | must be 1 or 2, else `-EINVAL` |
| `0x18` | `format.*` | 17-byte sub-struct: interleave, format, rate, voices, `int8 special` — memcpy'd into the handle with length `0x11` |
| `0x1C` | `format.format` | validated ≤ 15, width looked up |
| `0x24` | `format.voices` | multiplied by sample bytes |
| `0x1D8` | `start_mode` | copied into `handle[0x9A8 + ch*4]` |
| `0x1E4` | **`why_failed`** | zeroed on entry; on a failed devctl the driver's reason is copied back into the caller's struct before returning `-errno` |
| `0x1E8` | `buf.block.frag_size` | rate-scaled by the library |
| `0x1F0` | `buf.block.frags_max` | read |
| total | **752** (`0x2F0`) | the devctl `0xC2F04134` encodes a `0x2F0` transfer |

`stop_mode @0x1DC`, the `time`/`ust_time` word `@0x1E0` and `frags_min
@0x1EC` fill the gaps and match QSA's documented field order
**[unverified]**.

Also confirmed: `snd_pcm_plugin_set_disable(h, mask)` stores `mask` at
`handle[0x9B0]`, and `snd_pcm_plugin_params` tests `mask & 2` before building
the mmap plugin — so `PLUGIN_DISABLE_MMAP == 2` here, and the canonical QSA
recipe of disabling mmap before negotiating is followed.

## 10. Heavy GPU load can freeze the whole unit

Some GL loads freeze **the whole MMI**: picture, knobs, radio volume, our
own watchdog thread and run.sh, for minutes, until the reboot key combo.
No crash, no GL error. Details in [`gpu-results.md`](gpu-results.md).

| load | result |
|---|---|
| 8 lit tori (~8200 triangles, ~157 KB of client arrays per frame, ~20 fps) over the nav map, after ~33 s of other GL | **froze** after ~25 frames (§4.4 there) |
| stress scene alone over the HMI main menu: 8 tori for 60 s, then 12 tori (~235 KB/frame) | ok; free RAM and swap times flat |
| the next step, 16 tori (~314 KB/frame) | **froze on the first frame** (§8 there) |
| the full hello4 sequence (probes, three scenes, stress up to 12 tori) | ok, end to end (§9 there) |
| single texture uploads, each followed by `glFinish` (800×480 RGBA in 31 ms) | ok |
| several big texture uploads queued without a `glFinish` in between | **froze**, twice (§6 there) |

No single per-frame size explains all of these. A limit on the combined GPU
work in flight (ours plus the HMI's, e.g. NavCore drawing the map) fits
them **[hypothesis]**. Rules: keep per-frame client-array geometry well
under ~150 KB, keep at most one large upload in flight, and expect any GL
app to be able to take the unit down, not just itself.

## 11. Standing gotchas

- `/tmp` does not exist as a writable filesystem (`mkdir: /tmp: Function not
  implemented`); `run.sh` copies binaries there anyway and it works, because
  the path is provided by the loader, but `rm -rf /tmp/...` reports
  `Bad file descriptor`.
- There is no `sync` and no `touch` in the on-car shell. `run.sh` fakes
  `touch` with `: > file`.
- The unit's clock starts at `Jan 01 1970` on a cold boot, so file mtimes and
  any in-file "Generated" date are meaningless. Use the dump directory epochs.
- Screenshots and file writes are deliberately excluded from the frame
  statistics in hello2/3/4 (a `paused` accumulator), so fps and worst-frame
  measure drawing rather than I/O.

# MMI 3G+ platform notes — what the car has actually told us

Confirmed facts about this unit, gathered from real runs and from
disassembling the device's own libraries. Everything here has been observed;
anything still guessed is marked **[unverified]**. The point is that nothing
below needs re-testing — build on it.

Sources: `dev/10.10.2026 - first version run`, `dev/10.10.2026 - second
version run (old toolchain)`, `dev/10.10.2026 - second version run (new
toolchain O2)`, `dev/11.10.2026 - third version (3d pipeline)` (written up
in [`gpu-results.md`](gpu-results.md)), the later runs named in each
section (up to `dev/21.10.2026`), plus IDA/objdump on the firmware libraries in
`dev/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/mnt/ifs-root/usr/lib/`.

---

## 1. Toolchain

- Debian sid `gcc-16-sh4-linux-gnu` (16.2.0) driven as a QNX 6.3 SH4
  compiler by `sdk/shle-nto-gcc` produces binaries the MMI loads and runs.
  Proven by `hello` writing its file, and by every later app.
- **`-O2` is correct under GCC 16.** The GCC 12.2 `a == 0 && b == 0`
  miscompile is gone: `lmprobe`'s on-car canary read `MISCOMPILED` under the
  old toolchain and `correct` under GCC 16, in back-to-back runs on the same
  unit. See [`gcc-sh4-treg-bug.md`](gcc-sh4-treg-bug.md).
- The build-time canary (`sdk/canary.c`) stays, because the SDK image is
  `FROM debian:sid-slim` — a rolling base whose GCC 16.x can change under us.
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
- This killed `hello4` in probe `fog` (fixed by `gfx_fog_linear`: all 9
  probes pass since 2026-10-11), and there are more unguarded divides
  waiting in both the GL driver and libasound.

Full writeup, including how to decode FPSCR from a core file and how to
disassemble a stripped firmware `.so` from a `mapaddr`:
[`qnx-sh-fpu-div0.md`](qnx-sh-fpu-div0.md).

## 3. Layer manager and the VFB — solved

The reverse-engineered `showScreen` sequence is correct and deterministic.
`lmprobe` ran it end to end with `rc=0` at every step, twice, and its
`d-shot` came back **byte-identical between runs** (`d7d051ec…`).

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
Found from the 12.10 v2 phone video and confirmed in layermanagerV2 (P1001,
`CSurfacesManager`). Each displayable owns two surfaces and a front index
`cur`:

| call | what the server does | address |
|---|---|---|
| `getVfb` | marks `surface[!cur]` not ready and returns it: the **back** surface | `0x080508A4` |
| `updateVfb` | marks `surface[!cur]` ready, then `toggleSurfaces` flips `cur`: what you drew becomes the front, the old front becomes the next VFB | `0x080514EC`, `0x080508F0` |
| (displayable flag bit 3, `this[0xC] & 8`) | after the flip, blits the new front into the new back so the back starts out equal to the screen | `0x08051572` |

So a client must call `getVfb` again after **every** `updateVfb` and draw
into the sid it returns. A client that keeps drawing into the first sid only
gets its picture on every other update. The updates in between show the
other surface, which nobody ever wrote: the pink/beige vertical-band noise
over the top ~60 % of the screen. usb_tx_test redraws once a second, so its
screen alternated between its UI and that noise. hello6's text screens were
the second update after `screen_open` and showed only noise. One-shot users
(showScreen, lmprobe) update once, which is why they never saw it. The same
never-written surface is the "noise over the map" in the GL apps' `s-shot`s
(§7). `src/common/screen.c` now re-fetches the VFB after each update and
keeps both surfaces attached.

Because this is settled, `lmprobe` is no longer in the default `TESTS` — it
is still built and still copied to the card.

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
"0 is acceptable" and returns the first match, which is stencil-less — which
is why early runs logged `GL_STENCIL_BITS: 0` and hello4's stencil probe
reported SKIP. `gfx.c` now requests 8 bits and falls back if a layer format
has no such config.

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
is quantised. `GL_EXT_timer_query` is the way to measure it properly.

Also: `swap failures=0` and `glError=0x0000` throughout every run, in every
app. The EGL/GL path itself is not a source of risk.

## 7. Screen capture: only `glReadPixels` is trustworthy

**Corrected by the 2026-10-11 run** ([`gpu-results.md`](gpu-results.md) §2):
layer-manager captures never contain GL output. `s-shot 0` is the live HMI
(the clock on the map moves between shots; identical md5s came from shots
taken in the same minute), and the noise in a GL app's `d-shot` is the
never-written VFB of cid 40, not our frame tiled. GL draws on hardware
layer 7 through the gf 3D target, outside the layer manager. The table
below is the original reading, kept for the record:

| path | result |
|---|---|
| `d-shot` of a **CPU-written** VFB (lmprobe) | **correct**, byte-identical across runs |
| `d-shot` of a **GL-rendered** VFB (hello2) | **structured noise** — right frame (the horizon is in the right place), wrong pixel decode. Almost certainly an NV tiled/swizzled surface read as linear. |
| `s-shot 0` (whole display) | **stale** — in one run `hello2_sshot`, `hello3_pixels` and `hello3_plasma` all share one md5 |
| `glReadPixels` + `bmp_write_rgba` | **correct** |

So `d-shot` works; it is specifically GL-rendered surfaces that come back
scrambled. Use `glReadPixels` for anything GL-rendered. A pbuffer (see §5)
is the untried route to a clean offscreen capture. **[unverified]**

## 8. Input: `/dev/ipc/ch8`

The unit's only user input reaches us as 27-byte messages on `/dev/ipc/ch8`.
Each message is the current state of the controls: bytes 14–15 are always
`01 02` (byte 17 = `01` too), and a message with all control bytes zero
follows a button release. Decoded from hello5's stage 2 on 2026-10-21, a
fixed sequence done by the user against the full 27-byte dumps:

| byte | value | control |
|---|---|---|
| 6 | detents | main rotary knob **clockwise (right)** |
| 7 | detents | main rotary knob **anticlockwise (left)** |
| 4 | detents | volume knob **up** |
| 5 | detents | volume knob **down** |
| 3 | `0x04` | **MENU**? First of the two buttons on 21.10, but *second* on 19.10 v2, so the name is **unconfirmed**. Both times the press falls in the same second as the HMI's `OnOff new system state is 48` |
| 1 | `0x02` | **top-left soft key**? Second on 21.10, first on 19.10 v2: **unconfirmed** |
| 1 | `0x40` | another button, pressed twice after those (not in the sequence; which one is unknown) |
| 1 | `0x80` | a button seen on 12.10 v2 (unknown) |

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
- MMI3GApplication logs the same turns in the system log, one line per ch8
  message at the same moments: `[KEY] Encoder Key=0x10 Incr=±n` for the
  main knob (byte 6 = +n, byte 7 = −n) and `Encoder Key=0x11` for the
  volume knob (byte 4 = +n, byte 5 = −n), sizes included (`02` →
  `Incr=-2`). Checked on 12.10 v2, 13.10, 19.10 v2 and 21.10.
- Buttons are bitmasks in bytes 1 and 3, sent on press, then an all-zero
  message on release (~200–350 ms later).
- The HMI still acts on the knobs and buttons while our app is on screen
  (user's observation). Keeping input away from the HMI is open
  (`docs/TODO.md`). On 2026-10-25 one MENU press in hello7 brought the
  HMI's main menu up over our displayable, and it stayed on top for the
  rest of hello7's run (phone video): the HMI re-shows its own screens
  when it handles a button, our layer doesn't come back by itself.
- Bytes 16–26 are **state, not controls** [inferred]: byte 16 was `00` on
  21.10 and `03` in every message on 25.10, byte 26 `11` all of 25.10,
  and bytes 23/25 changed with no control touched (around the MEDIA step
  of hello5). hello7 no longer decodes them. 25.10 also brought 29- and
  33-byte messages with byte 14 = `02`, and unnamed button bits byte 1
  `0x04`/`0x10`/`0x20` and byte 0 `0x01`/`0x02`/`0x08`/`0x10`/`0x40`
  (src/hello7/README.md).
- **Byte 15 looks like the unit's power state** [inferred]. It was `02` in
  all 282 messages logged up to 28.10. On 2026-10-28, with nothing
  touched, it went `02` (in a 29-byte message with byte 14 = `02`) →
  `03` 0.6 s later → `00` 3 s later, and then the run ended:
  hello5's log, `run.log` and the next app all stopped there. At the
  first of these messages the phone video has a loud click, the radio
  stops, and a tone falls from ~1 kHz to ~700 Hz over 2.5 s, like a motor
  running down. The car most likely switched off (ignition off or the
  unit going to standby), and the SD writes stopped with it.
  If that holds, ch8 gives an app a few seconds' warning to save its state
  and exit. To confirm: switch the ignition off on purpose during a run
  and compare.

**No key or button stops an app.** Runs end on their own timer, a signal, SD
removal or the watchdog — a stray press during a test cannot cut it short.
`quit.c` still reads ch8, logs the first messages raw and offers the bytes to
an app observer (`quit_init_ex`), which is how hello5 counts volume detents.
The knob and button log that decoded the table above was hello5's stage 2;
it is **hello7** now, a free-form tester that names every control as it is
used (`src/hello7/keys.c`) and reports unknown bits by byte, so the
unknown `0x40`/`0x80` buttons can be named with one press each.

## 9. Audio (QSA / libasound) — playback works

`snd_pcm_open` on card 0, device 5 (`tones1`, the UI-tone mixer input)
succeeds, and since the 12.10 v2 run so does the rest: `snd_pcm_plugin_params`
accepted 44.1 kHz mono S16_LE with the corrected enums, and 15 full 52,920-byte
`snd_pcm_plugin_write`s went through in 30 s. **Nothing was heard** in the
car (confirmed by the user after the 12.10 v2 run).

Why, probably: the FPGA playback streams go to the amplifier over MOST, and
the stock MMI does not just write to them. MMI3GApplication's
`CHBGWavePlayer` plays the system tones on `/dev/snd/tones1` too, but the
binary also names audio sources such as `eSRC_SYSANN_BEEP` and
`eAmxRingerToneHFP`, so the audio manager presumably connects a source in
the amplifier before its stream is audible. **[unverified]**

**13.10: all six devices play, none is heard.** hello5 played group N = N
chimes on each device (`5,1,2,3,4,0` = tones1, navi, SDS, BT-Tr, NAD, ent3)
with the radio on. Every chime was written in full and consumed in real time
(write + EBUSY ≈ 600–650 ms). The system log has an `hb_fpga: Play buffer
underrun strmN` for each group as its stream ran dry (strm1…strm4, then
strm0), plus `Audio Buffer Failed to Drain` at each close. Nothing was
heard. So io-audio → FPGA works, and the block is downstream. io-audio's
`hb_fpga` mixer has only PCM and I/O elements, no volume or mute, so the
gating is not in io-audio. Most likely the amplifier only mixes a stream in
once MMI3GApplication's audio management (`CAmpLC`, `LC_SRCSYSANN3_BEEP`,
`eSRC_SYSANN_BEEP`) has connected that source. **[inferred]**

The next hello5 tries the **io-media mixer** instead. It runs on the DM642 and
is what the MME plays music into (`mixer:DSPOUT_TDM1_51_SAD?delay=130` is
its `defaultoutput`). MMI3GApplication also builds mixer graphs from
`mixer:DSPIN_TDM1_20` (presumably the tuner) to the `DSPOUT_TDM1` outputs.
So a graph attached to a TDM1 output should be mixed into whatever is
playing. Groups 1–3 play through the unit's `testapp-file` on mixer outputs
TDM1 5.1, TDM1 2.0 and TDM2 2.0. Groups 4–6 are tones1, navi and ent3 as
controls (outputs `m0,m1,m2,5,1,0`).

**19.10: two mixer outputs play, none is heard, one cuts the radio.** With
the radio on: `DSPOUT_TDM1_51_SAD` would not attach ("Mixer entry for
DSPOUT_TDM1_51_SAD: Already in use", so the MME's own player holds it).
`DSPOUT_TDM1_20` and `DSPOUT_TDM2_20` played every chime to `INPUT_EOF`.
Nothing was heard, but **group 2 (TDM1 2.0) stopped the radio for about a
second**. tones1, navi and ent3 behaved as on 13.10.

io-media-nvidia's mixer (efs-system, strings at `0x237ed4`) knows the
outputs `DSPOUT_TDM1_51(_SAD)`, `TDM1_20(_SAD)`, `TDM2_20(_SAD)`,
`TDM3_20(_SAD)` and `PCI1..3`; the inputs `DSPIN_TDM1..3_20`; and four
player zones `NV_AUDIO_MIXER_IN_ZONE_1..4`. Each output takes one player
("mixer entry"), and the DSP mixer can stamp a "SAD" header on a stream
(`AMixerSadHeaderSet`).

Reading [inferred]: TDM1 carries the main entertainment stream to the
external MOST amplifier: radio (`DSPIN_TDM1_20`, routed by MMI3GApplication's
mixer graph) and MME music both go out as `TDM1_51_SAD`. `TDM1_20` drives
the same TDM port in another format, so while it played, the stream the
amplifier was decoding was gone (the radio stopped), and our 2.0 stream
without the header was not played either. Every other stream (TDM2/TDM3,
the io-audio devices) reaches the amplifier on channels it only mixes in when
MMI3GApplication's audio management switches that source on. Next hello5
(default `m2,m1,m6,m4,m7`): TDM1 2.0 *SAD*, TDM1 5.1 (no SAD), TDM3 2.0 SAD,
TDM2 2.0 SAD, TDM3 2.0. The user also notes which groups cut the radio.

**19.10 v2: all five mixer outputs play; maybe one chime heard.** Outputs
`m2,m1,m6,m4,m7` (TDM1 2.0 SAD, TDM1 5.1, TDM3 2.0 SAD, TDM2 2.0 SAD, TDM3
2.0): every one attached, with no "Already in use" this time, and played every
chime to `INPUT_EOF` (testapp-file exits after 640–700 ms for the 600 ms
clip). The listener thinks one chime was heard near the start, group 1 or
2, but could not say which. The ch8 log backs that up: the first
volume-knob turns came 1.3 s after group 1's only chime (TDM1 2.0 SAD) and
went on through group 2. Groups 3–5 (TDM2/TDM3) were not heard. So the
candidate is **TDM1 2.0 SAD** (one chime heard = group 1), with TDM1 5.1
second **[unverified]**. The next hello5 plays the same outputs with a
2.5 s chime (`sdk/mkbeep.py`), long enough to place by ear, and the user
again notes which groups stop the radio.

**21.10: TDM1 2.0 SAD is heard; TDM1 5.1 mutes the car.** Same five
outputs, 2.5 s chime, radio on. io-media treated all five the same: each
chime attached, played to `INPUT_EOF`, and `testapp-file` exited after
2.46–2.57 s. What the user heard:

| group | output | heard |
|---|---|---|
| 1 | m2 `DSPOUT_TDM1_20_SAD` | **the chime, clearly** |
| 2 | m1 `DSPOUT_TDM1_51` | no chime; **the car audio went silent** while it played |
| 3 | m6 `DSPOUT_TDM3_20_SAD` | nothing |
| 4 | m4 `DSPOUT_TDM2_20_SAD` | nothing |
| 5 | m7 `DSPOUT_TDM3_20` | nothing |

The slog has nothing from io-media or the audio stack around any group.

Reading [inferred]: the amplifier decodes the TDM1 stream with its SAD
header. 2.0 with the header (`TDM1_20_SAD`) is a format it accepts, so our
chime was played. 5.1 without the header
broke the stream, so it went silent (as `TDM1_20` did on 19.10). TDM2 and
TDM3 stay unheard in every format. **For an app that wants sound, the
way out is `mixer:DSPOUT_TDM1_20_SAD?delay=130` through io-media** (as the
unit's `testapp-file` does). Not known yet: whether it mixes with the radio
or replaces it while playing, and what it does while the MME plays music
(the MME holds `TDM1_51_SAD`).

**What `TDM1_20_SAD` does next to the car's own audio.** hello5 runs
steps instead of the group survey (steps `mntv` since 25.10, `mnvt`
before; the survey is step `g`). Each step plays a ping train, a 0.5 s ping every 2 s, on
`TDM1_20_SAD`. hello5 renders the train itself (`src/hello5/pulse.c`) into
`/tmp`, so the run also shows whether io-media plays an app's file from
RAM. If it doesn't, the step retries from the card. The silences between
the pings carry the answer:

| step | the driver | answers |
|---|---|---|
| m | radio on | is the radio still playing between the pings (mixed), or not (replaced)? |
| n | radio on; hello5's own io-media client plays 9 pings at its own volume 100 / 30 / 100, then 3 one-ping files queued | does an app's own volume work? are the queued files gapless? when does io-media say it is playing? |
| t | presses MEDIA and starts music | does the output still attach while the MME plays, and does the music keep playing between the pings? |
| v | turns the volume knob 3 down, then 3 up, during the pings | do the pings follow the car's volume? (clicks counted on screen and logged) |

**2026-10-25 (steps `mnvt`), from the phone video's sound** (the listener
heard only step `m`; the 880 Hz pings were found in the recording at the
times the screen showed them):

- `m`: all 6 pings audible, ~15 dB over the cabin noise; `testapp-file`
  plays our file from `/tmp`. The radio isn't audible in the video, so
  mix vs replace is still open.
- `n`: all 12 pings audible at `m`'s level: **hello5's own io-media client
  (`iom.c`) plays sound** through `TDM1_20_SAD`. STATUS playing +24 ms
  after play, first TIME +480 ms. Volume 30 changed the level by ~3 dB at
  most (not conclusive; the next run uses 0). Queued files play, but
  **each adds ~350 ms of silence**: no gapless queueing by
  `INPUT_EOF`/attach. io-media sends **one `FINISHED` per queue**, not per
  file; hello5 misread that and replayed the last file (fixed).
- `v`: ping 1 audible, then the knob was turned fast (148 detents down,
  37 up counted) and pings 2–10 were gone: following the volume, or the
  volume at ~0, can't be told apart.
- `t`: no pings and no music audible, most likely the volume still down
  from `v`. Hence the new order, `v` last.

**2026-10-28 (steps `mntv`, ended during `t`'s countdown), from the phone
video's sound** (`src/hello5/README.md` has the details). The radio was
audible this time, music with a bass beat:

- **`TDM1_20_SAD` replaces the radio, it doesn't mix.** For the whole
  12 s of step `m`'s file, everything between the pings is at the
  recording's noise floor (15–25 dB below the radio). The radio comes back
  ~0.2 s after the file ends. In step `n` the radio's bass alone faded
  back in from ~10 s on, under the pings. That is unexplained.
- **The graph's volume `devctl` has no effect on this output.** At volume
  0, pings 4–6 were as loud as the rest (±0.5 dB). Volume 30 on 25.10
  read the same. Candidate: the mixer URL's `volume_ctrl` key
  [unverified].
- **Each queued file adds ~350 ms of silence** (2.38/2.39/2.33 s instead
  of 2.0), the same as on 25.10.

What this means for an app with sound: it can play through
`TDM1_20_SAD`, but the user's radio or music pauses while it does, and
its level is the car's volume. That suits short prompts and a
full-screen media app, but not background sound over the radio.

**io-media from our own code (from `testapp-file`'s disassembly, P1001
efs-system).** `testapp-file` links no io-media library (NEEDED is only
`libc.so.2`). It creates a graph by opening
`/dev/io-media/graphs/trackplayer/<name>` (`O_RDWR|O_CREAT|O_EXCL`, 0626)
and drives it with `devctl`s: attach the output URL, set the volume (0–100,
plus balance and fade), attach an input (`file:`, `tmpfile:` or a plain
path; the extension picks the media type, `.wav` = 3), play, and read
events from the same fd. Closing the fd destroys the graph. The codes and
payloads are in [`src/hello5/iom.h`](../src/hello5/iom.h), and hello5's step
`n` is the first use of them. **[confirmed in code, untested on the car]**

- `testapp-file` always exits with **status 1**. `main` has one epilogue
  (`mov #1,r0` at `0x80412A4`), and every path, success included, ends
  there. Read its output instead: success is `FINISHED … 0 left to do`
  followed by `Done, destroying the graph`.
- Its stdout is **fully buffered** when it is a file: no `setvbuf` or
  `fflush`, and QNX's `BUFSIZ` is 1 KB. So its log lines carry no timing,
  and stderr lines can come before stdout lines printed earlier. hello5
  step `n` logs every event itself, with the time since play.
- Its multi-URL mode chains files: on `INPUT_EOF` (the file has been read,
  which comes early: before 0.4 s of a 2.5 s clip) it detaches the input
  and attaches the next. On `FINISHED`, if files are left, it plays again.
  Step `n` copies this, to see whether an app can queue sound without gaps.
- The mixer URL's query keys are `delay`, `volume_ctrl`, `mime`/`mimetype`,
  `EncodingBitrate` and `PriorityAdjust`. There is no `volume=`.
- `_SAD` is the same TDM port with flag `0x40000` in io-media's output
  table at `0x82A8488` (`TDM1_20_SAD` `0x40002`, `TDM1_20` `0x2`). It
  presumably switches on the DSP's `AMixerSadHeaderSet`, the header the
  amplifier wants **[inferred]**.

Until 13.10, stage 1 played group N = N chimes on each io-audio playback
device in turn, so the number of chimes heard identifies the device. For every chime
it logs the write time plus how long `prepare` keeps returning `EBUSY`: about
600 ms (the clip's length) means the device consumed it in real time, heard or
not. Each group runs on its own thread with a deadline, so a device whose
write never returns is written off instead of hanging the run. The
screen (`src/hello5/h5ui.c`) shows the group in its own colour, `GROUP n`,
`chime k of n` and a white PLAYING box, presented just before the write
that starts each chime, plus the log clock, so a phone video with sound
says which chime was heard; the log line of each chime gives the `t=` its
PLAYING frame went up. Stage 2
logs every ch8 message in full against a fixed sequence of knob turns and
presses (section 8).

- `snd_pcm_plugin_prepare` returns **`EBUSY` while the previous clip is still
  playing**. On 12.10 v2 every EBUSY came less than 600 ms (the clip's length)
  after a successful beep. That is not a failure; hello5 now counts these
  separately.
- The setup read-back (`snd_pcm_channel_setup_t`) has **no `sync[]` field**:
  `snd_pcm_plugin_setup` (libasound `0xC378`) memcpys the 17-byte format block
  to **`@0x08`**, not `@0x18` like params. Reading it at `0x18` is why the run
  logged "negotiated 0 Hz, 0 ch". The header is fixed and asserted.

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
recipe of disabling mmap before negotiating is now followed.

The first 12.10 run failed with `why_failed=2` = `BAD_START`: the enum values
had been copied from ALSA 0.5. They are now read from io-audio (`0x0804E780`).

## 10. A heavy 3D load froze the whole unit

On 2026-10-11 hello4's stress scene (8 lit tori, ~8200 triangles from
client-side arrays, ~20 fps) froze **the whole MMI**: picture, knobs, radio
volume, hello4's own watchdog thread and run.sh, for minutes, until the
reboot key combo. No crash, no GL error. Cause still open; analysis and the
follow-up experiment are in [`gpu-results.md`](gpu-results.md) §4.4. Until
then, keep GL geometry far below that, and expect any GL app to be able to
take the unit down, not just itself.

On 2026-10-19 the stress scene alone, over the HMI main menu, passed 8 tori
for 60 s and 12 tori, then froze the unit on the first 16-tori frame
(~314 KB of client arrays per frame). Free RAM and swap times were flat up
to the end. Together with 11.10 (frozen at 8 tori over the nav map), this
points to a limit on the combined GPU work in flight **[hypothesis]**
([`gpu-results.md`](gpu-results.md) §8). On 2026-10-21 the whole hello4
sequence (probes, three scenes, stress up to 12 tori) ran clean, so that
load is safe ([`gpu-results.md`](gpu-results.md) §9).

On 2026-10-12, hello6 froze the unit twice in the same way with **texture
uploads** ([`gpu-results.md`](gpu-results.md) §6–7). The working pattern so
far: single uploads, each followed by `glFinish`, are fine (800×480 RGBA in
31 ms). Letting several big uploads queue up without a `glFinish` in between
froze the unit both times. Keep at most one large upload in flight.

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

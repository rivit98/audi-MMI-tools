# MMI 3G+ platform notes — what the car has actually told us

Confirmed facts about this unit, gathered from real runs and from
disassembling the device's own libraries. Everything here has been observed;
anything still guessed is marked **[unverified]**. The point is that nothing
below needs re-testing — build on it.

Sources: `dev/10.10.2026 - first version run`, `dev/10.10.2026 - second
version run (old toolchain)`, `dev/10.10.2026 - second version run (new
toolchain O2)`, `dev/11.10.2026 - third version (3d pipeline)` (written up
in [`gpu-results.md`](gpu-results.md)), plus IDA/objdump on the firmware libraries in
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
(§7). `src/usb_tx_test/screen.c` now re-fetches the VFB after each update and
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
Decoding so far, from the raw dumps hello5 logged:

```
00 00 00 00 00 00 01 00 00 00 00 00 00 00 01 02   main knob, one way
00 00 00 00 00 00 00 01 00 00 00 00 00 00 01 02   main knob, the other way
00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 02   button press
```

Byte 6 and byte 7 carry main-rotary detents, one per direction; bytes 4/5 are
the volume knob **[unverified]**; all of bytes 4–7 zero is a button.

One 30 s session produced 20 clean detent events, so the channel is reliable.

**No key or button stops an app.** Runs end on their own timer, a signal, SD
removal or the watchdog — a stray press during a test cannot cut it short.
`quit.c` still reads ch8, logs the first messages raw and offers the bytes to
an app observer (`quit_init_ex`), which is how hello5 sees detents.

## 9. Audio (QSA / libasound) — playback works

`snd_pcm_open` on card 0, device 5 (`tones1`, the UI-tone mixer input)
succeeds, and since the 12.10 v2 run so does the rest: `snd_pcm_plugin_params`
accepted 44.1 kHz mono S16_LE with the corrected enums, and 15 full 52,920-byte
`snd_pcm_plugin_write`s went through in 30 s. Whether the chime was
**audible** has not been confirmed: the phone video has no sound track.

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

# GPU results — what hello2, hello3 and hello4 told us

Conclusions from the 2026-10-11 car run (`dev/11.10.2026 - third version
(3d pipeline)`) plus a phone video of the screen during the whole run
(`VID_20261011_174053.mp4`, 120 fps, kept outside the repo). The earlier runs
(`dev/10.10.2026 - …`) are used for comparison. The platform facts these
build on are in [`platform-notes.md`](platform-notes.md).

Conditions: GCC 16 `-O2` build, MMI cold-booted, the HMI showing the
**navigation map** underneath the whole time, car parked. Run order: hello2,
hello3, hello4; hello4 froze the unit, so hello5/hello6/usb_tx_test did not
run.

| App | Verdict | In short |
|---|---|---|
| hello2 | OK | GL bring-up, 58 fps animation, clean hand-back to the HMI |
| hello3 | OK | first trustworthy fill-rate numbers |
| hello4 | — (froze the unit) | 9/9 probes pass, 3 scenes pass, then the **whole MMI froze** in the stress scene |

hello2 and hello3 have answered their questions and are no longer in the
default run. hello4 ran only its stress scene on 19.10 (see §4.4, §8).
On 21.10 it ran everything, with the stress steps stopping at 12 tori, and
passed (§9). It has left the default run too.

---

## 1. The GL bring-up is settled

Every step of `gfx_init` returned success in all three apps, identically:
layer manager 1.1, `gf` device `nv`, 800×480 BGRA8888 at 60 Hz, layer 7,
VFB `paddr=0xdeabc000` stride 3200, EGL 1.3, config 1 (8888, depth 24,
stencil 8), `GL_RENDERER NVIDIA EMBEDDED Build 6.0180/NV18`.
`glError=0x0000` and `swap failures=0` in every per-second line of every app.

- **`alpha_sid` keeps changing**: 12 (hello2), 24 (hello4) this run, after
  16/22/28/34 before. Never hardcode it.
- **Hand-back to the HMI works.** Teardown (`updateVfb` → unregister →
  show `{40,16,19}`) returned 0 every time, and the video shows the nav map
  back on screen after each app and between the `showScreen -t` status
  screens. That was "not yet verified" in the README; it is now.

## 2. Screen captures: only `glReadPixels` sees what GL draws

| Capture | What it actually contains |
|---|---|
| `glReadPixels` → `hello2_glread.bmp` | **Our frame**, correct: teal background, the colour square, the white block, the yellow bar |
| `s-shot 0` (all six of this run) | The **live HMI** (nav map, clock moving 17:42 → 17:44) with the same noise pattern over it every time |
| `d-shot 40` (`hello2_dshot.png`) | The same kind of noise, no trace of hello2's picture |

`hello2_sshot.png` and `hello4_world.png` differ by **0.1/255** on average,
although hello2 drew a flat-coloured square and hello4 a textured 3D world.
So the layer manager's captures never contain our GL output, and the noise
over the map is not our frame scrambled. GL draws through the `gf` 3D target
straight onto hardware layer 7. The layer manager composites, and captures,
only its own displayables. For cid 40 that is a VFB buffer GL never writes,
so it contains whatever was in that memory before.

This corrects two readings in `platform-notes.md` §7:

- "`s-shot` is stale": it is live. Earlier shots had the same md5 because
  they were taken within the same minute, so the map was identical.
- "`d-shot` of a GL VFB is our frame, tiled/swizzled": hello2 has no
  horizon, so the "horizon" in that d-shot is not ours.

Consequences:

- Use `glReadPixels` for every GL screenshot.
- Drop `s-shot`/`d-shot` from the GL apps. Each `s-shot` cost **5–6 s**
  (hello3/hello4) and the `d-shot` about 3 s, for nothing.
- Writing the 1.1 MB BMP took about 8 s. SD writes run at roughly
  **140 KB/s**, so large files during a test are expensive.

## 3. hello3: fill rate

### 3.1 The 10.10 numbers were measurement artefacts

On 10.10 the pixels scene showed 19 fps and plasma "0 fps". The cause was the
mid-scene screenshot (9.7 s) being counted inside the scene, which squeezed
ten per-second reports into a few hundred ms. Since captures are kept out of
the scene clock, the same scenes measure:

| scene | 10.10 (with capture inside) | 11.10 |
|---|---|---|
| pixels (1 textured full-screen quad, `GL_NEAREST`) | 286 frames / 14.6 s | **580 / 10.0 s = 57 fps** |
| plasma (3 blended, texture-matrix-animated layers) | 10 frames / 10 s | **388 / 10.0 s = 38–40 fps** |

The GPU did not get faster. The 11.10 numbers are the first ones without
capture time mixed in.

### 3.2 Fill-rate table

| layers | 10.10 fps | 11.10 fps | vsyncs per frame (60 Hz) | reported Mpixel/s |
|---|---|---|---|---|
| 1 | 58.1 | 57.4 | 1 | 22 |
| 2 | 46.0 | **57.5** | 1 | 44 |
| 4 | 29.0 | 29.6 | 2 | 45 |
| 8 | 19.7 | 19.5 | 3 | 60 |
| 16 | 14.8 | 14.8 | 4 | 91 |
| 32 | 9.7 | 10.7 | 5–6 | 132 |

The table is vsync-quantised, as `platform-notes.md` §6 says, but it now pins
the frame budget down:

- **2 full-screen textured, blended layers fit in one 60 Hz frame; 4 do not.**
  The 3-layer plasma at ~39 fps (1.5 vsyncs on average) sits in between.
- A rough model, frame = fixed cost F + N × per-layer cost L, fits all six
  rows with **F ≈ 10 ms** and **L ≈ 2.8–3 ms** (≈ 130–140 Mpixel/s per
  800×480 layer). F is not measured directly: it covers clear,
  `glFinish` + `eglWaitGL`, the swap and the layer-manager side. The
  2-layer run going from 46 to 57.5 fps between runs suggests F sits close
  to the edge where 2 layers still fit in a frame. `GL_EXT_timer_query`
  would measure F and L properly.
- In practice, for a 60 fps UI: about **6 ms of GPU fill per frame**, which
  is two full-screen blended layers, or one plus a lot of small quads.

### 3.3 Frame pacing

Nearly every per-second line in all three apps has `worst frame` = 50 ms (3
vsyncs), even at trivial load (hello2: 58–59 fps). The most likely cause is
our own per-second log line, `fsync`'ed to the SD card inside the render
loop (~12 ms per line, from the spacing of consecutive log lines). It is
only a likely cause: the HMI or the layer manager could also take a slice.
A render loop that has to stay smooth should log from another thread or not
`fsync` per line.

## 4. hello4: the 3D pipeline works; heavy geometry froze the unit

### 4.1 Probes: 9/9 PASS

The FPU fix (`gfx_fog_linear`, [`qnx-sh-fpu-div0.md`](qnx-sh-fpu-div0.md))
works: the 10.10 run died with SIGFPE in probe `fog`; this one passed all of
them, each read within a few units of the exact spec value.

| probe | notable reads |
|---|---|
| clear, depth, cull, persp | exact |
| light | 255 / **180** / 0: 180 = 255 × cos 45°, so per-vertex diffuse lighting matches the spec exactly |
| texture | all four texels exact (`GL_NEAREST`, `GL_REPLACE`) |
| fog | full / half = **127 127 128** (spec 127.5) / none |
| blend | 153 **179** 204 (spec 178.5) |
| stencil | **passes**: the 8-bit stencil config works (it was SKIP before stencil was requested) |

So depth (24-bit), culling, `glFrustumf`, lighting, texturing, fog, blending
and stencil all behave as ES-CM 1.0 says. Float arguments cross the library
ABI correctly.

### 4.2 Scenes 1–3 pass

| scene | content | fps |
|---|---|---|
| cube | 2 cubes, 24 triangles, vertex colours, depth test | 57 (574 frames / 10 s) |
| light | 1 torus (1024 tris) + 2 lights + specular, smooth then `GL_FLAT` | 52 (49–58 per second; **flat and smooth cost the same**) |
| world | ground 1152 tris + 8 crates + torus ≈ 2300 tris, mipmapped textures × lighting × fog | **28** (exactly 2 vsyncs) |

The screenshots of these scenes are useless (see §2), but the video shows all
three rendering correctly: the cubes intersect cleanly, the specular
highlight moves, and the checkerboard has no moiré in the distance, so
mipmapping is in use.

### 4.3 Geometry is the expensive part

Stress scene, lit untextured tori (1024 triangles, 561 vertices each). The
freeze came before the triangle-rate table was written, so these come from
the per-second lines. The 1.43 s steps don't line up with the seconds, so the
2- and 8-tori values are what is left of a mixed second after taking out the
part spent on the neighbouring step:

| tori | triangles/frame | fps | ≈ lit triangles/s |
|---|---|---|---|
| 1 | 1 024 | 53 | 54 k |
| 2 | 2 048 | ≈ 30 | 61 k |
| 4 | 4 096 | 29 | 119 k |
| 8 | 8 192 | ≈ 20 | 160 k |

- A GeForce4 MX does millions of hardware-T&L triangles per second; this
  path manages **under 0.2 M/s**, and the per-frame cost does not grow
  linearly (2 and 4 tori both take 2 vsyncs). Vertices come from client-side arrays, so
  the driver copies every vertex (24 B: position + normal) every draw. Most
  likely the transform/lighting or that copy runs on the 792 MHz SH4, not
  on the GPU.
- **Practical budget for 60 fps: under about 1000 lit triangles per frame.**
  Even the single torus does not hold 60.
- The lever not tried yet: `GL_OES_vertex_buffer_object` is advertised, but
  `glBindBuffer` is not exported by `libGLES_CM.so.6`. Whether
  `eglGetProcAddress` hands it out is untested.

### 4.4 The freeze

**What happened.** About 5.5 s into the stress scene, drawing 8 tori, the
picture stopped. It stayed frozen for **at least 3 minutes**. The radio volume
knob did nothing, and only the reboot key combo brought the unit back.

**Timeline**, from `hello4.log` and the 120 fps video:

| when | event |
|---|---|
| log 52 020 ms | last line: `t=35 s stress fps=22 tori=8 glError=0x0000` (the second was ~30 % 4 tori, ~70 % 8) |
| video +0.00 s | HUD changes to "22", the frame after that line |
| video +0.5 s | **last frame that changes**; the rest of the video is this frame |
| (+0.71 s) | the switch to 16 tori was due here; it never showed |
| (+1.0 s) | the next per-second log line was due; never written |
| (+28 s) | hello4's own watchdog (`alarm`, 80 s from start) would have logged `got signal 14`; never written |
| (+78 s) | run.sh's 130 s timeout would have logged `stopped after 130 s (TERM)`; never written |

`gfx_swap` does `glFinish()` + `eglWaitGL()` before every swap, so the CPU is
never more than about one frame ahead of the screen. The freeze therefore
happened while drawing ordinary 8-tori frames:

- **not at the step change** (that was due 0.2 s later),
- **not in an SD write** (none was due, and one line per second had been
  fine for 50 s),
- **not after a GL error** (`glError` 0 every second, no swap failures),
- **not a crash of our process.** A SIGFPE/SIGSEGV is caught by the dumper
  and the system carries on, as on 10.10.

**The whole unit stopped, not just hello4.** hello4's watchdog runs in its
own thread and should have fired after 28 s. run.sh is a separate process.
The HMI and audio are separate processes again. None of them did anything
for minutes. hello4 runs at the default priority (10). A normal-priority
process cannot starve every higher-priority thread in the system on its own,
so the stall is in privileged code or in hardware. Two candidates:

- the nVidia driver spinning with interrupts off, or at the top priority;
- the GPU wedging the bus or memory controller, so the CPU stalls on an MMIO
  or uncached access.

The QNX system log was in RAM and was lost with the reboot.

**Hypotheses, most likely first:**

1. **GPU or driver hang under sustained geometry load** from client-side
   arrays. 8 tori push about 108 KB of vertex data through the driver per
   frame, 22 times a second. It seems tied to load rather than to an exact
   torus count: it struck mid-step, after about 25 frames at 8 tori. hello4
   had been drawing for about 33 s by then (50 s minus the screenshots).
2. **Contention with the HMI's own GPU use.** The nav map was on screen, and
   NavCore draws its map with the same GPU. hello2/hello3 ran over the same
   map without trouble, but their geometry was trivial.
3. **Something building up in the driver** (memory, handles) over the 50 s
   of GL, reaching a limit at a load-dependent moment.

**What the next run does** (`sd/apps.conf`, options in the README):

```
hello4 <sd> scenes=s step=5 tori=1,2,4,8:60,12,16,24,32,64 trace=1
```

Only the stress scene runs (the probes and scenes 1–3 have passed): 1, 2
and 4 tori for 5 s each, then **8 tori held for 60 s**, then 12, 16, 24, 32
and 64. It logs:

- a line at the start of every step: tori, triangles per frame, free RAM;
- one fsync'ed line per frame (`f<frame> n=<tori> draw=<ms> swap=<ms>`), so
  the last line on the card is the last frame that finished;
- free RAM in every per-second line (the size of `/proc`), for hypothesis 3.

hello4 now runs after hello5 and hello6, so a freeze only costs usb_tx_test.

How to read the result:

| outcome | meaning |
|---|---|
| freezes during the 8-tori hold | load-related, not a torus-count threshold. Look at whether `swap=` grows before the end, and whether free RAM drops |
| survives 8, freezes at a higher count | a load threshold. Its torus count and frame are in the log |
| survives everything | the per-frame trace (~12 ms of SD I/O per frame, during which the GPU is idle) may hide it. Rerun with `trace=0`. Also try with the HMI on a non-map screen (radio), for hypothesis 2 |
| frozen picture, but knobs and volume still work | only our process or the GPU is stuck, not the CPU. The watchdog lines should then appear |

Until this is understood, **any GL app should keep geometry well below 8000
lit triangles per frame** and should not run sustained heavy 3D on this
unit. Text, quads, textures and blending (hello2, hello3, hello4 scenes 1–3)
were all fine.

## 5. Summary for app design

- GL bring-up, teardown and hand-back are reliable. The whole ES-CM 1.0
  fixed-function pipeline works to spec (§1, §4.1).
- Budget per 60 Hz frame: about 2 full-screen blended layers of fill (§3.2),
  and under about 1000 lit triangles from client arrays (§4.3).
- Vertex-heavy 3D is slow and has frozen the unit (§4.4).
- Capture with `glReadPixels` only. Layer-manager shots show the HMI, not GL
  (§2).
- Keep SD writes and `fsync` out of the render loop: ~12 ms per line,
  ~140 KB/s (§2, §3.3).

## 6. 12.10: hello6 froze the unit on its first texture upload

Run `dev/12.10.2026 - hello5 hello6`. hello6 got through sysinfo, the CPU
benchmarks and the VFB tests, then the **whole MMI froze again** (as in
§4.4). The last fsync'ed line was `RESULT vfb_memcpy_out`, so the freeze is
somewhere in `lm_update_vfb` → GL state setup → `upload_case("rgb565")`:
`glTexImage2D(GL_RGB 565, 1024x512)` from a zero buffer, then
`glTexSubImage2D` 800x480 in a ~400 ms loop. That was the first
`glTexSubImage2D`, the first non-square / non-RGBA texture and the first
~1 MB upload any of our apps made (hello3/hello4 only did square RGBA
`glTexImage2D` at init).

- Not video memory: `ShowVideoMemory` said 123.7 of 134 MB free just before.
- Both freezes (hello4 vertex arrays, hello6 texture upload) push a lot of
  client data through the driver. **[hypothesis]** A whole-system freeze fits
  `devg-NVMiniRM`'s thread 2, which runs at **priority 21 FIFO**, spinning on
  a wedged GPU: everything at or below 21 (the HMI at 10, our watchdogs,
  run.sh) would starve, while io-audio's interrupt thread (63) keeps going.
- hello6 now runs the GPU phase **last** (after video) and logs a `gl:` step
  line before every data-moving GL call, with each format's first upload
  growing 64x64 → 256x256 → 800x120 → 800x480. RGBA goes first. The last
  `gl:` line on the card names the call, size and format.

Also from this run: `RESULT ram_memcpy: 860527 MB/s` was GCC dropping the
copies into a buffer nobody read; fixed with a compiler barrier.

## 7. 12.10 v2: single uploads pass, back-to-back uploads freeze

Run `dev/12.10.2026 hello5 hello6 v2` (phone video `VID_20261012_173526`,
kept outside the repo). The GPU phase ran last, after video. It got further
than v1, then froze the whole unit again. Last lines on the card:

```
gl: lm_update_vfb(40)                                  rc=0
gl: upload_rgba8888: glTexImage2D 1024x512             (+ glFinish)
gl: upload_rgba8888: glTexSubImage2D 64x64 + glFinish     ok in 0 ms
gl: upload_rgba8888: glTexSubImage2D 256x256 + glFinish   ok in 5 ms
gl: upload_rgba8888: glTexSubImage2D 800x120 + glFinish   ok in 8 ms
gl: upload_rgba8888: glTexSubImage2D 800x480 + glFinish   ok in 31 ms
gl: upload_rgba8888: timed loop                        <- frozen here
```

The timed loop called `glTexSubImage2D` 800×480 RGBA (1.5 MB) on the same
texture over and over, with a `glFinish` only after every 4th call. The
same upload alone, followed by `glFinish`, had passed a moment earlier. The
log is fsync'ed per line and the loop lasts ≥ 400 ms, so the freeze is
inside that loop. hello6's 600 s watchdog and run.sh's 720 s timeout never
fired, and hello4 never started: the whole unit stopped, as in §4.4 and §6.
(The screen was already black then, because sysinfo's `ShowDisplayables 0`
had emptied the shown list; see platform-notes §3. So the video shows no
frozen picture, only no further change.)

Both 12.10 freezes let several big uploads queue up between `glFinish`
calls: v1 had a `glTexImage2D` 1 MB, then a loop of 768 KB RGB565 sub-uploads
with no `glFinish` at all. Everything that was followed by a `glFinish` on
its own passed.

**[hypothesis]** The driver (or GPU) hangs when a texture upload is queued
while an earlier one, to the same texture, is still in flight. The
alternative is a hang once the queued client data passes some staging-buffer
size. hello4 (§4.4) does `glFinish` every frame but froze with about 108 KB
of vertex data per frame. That suggests the risk grows with the work in
flight rather than having a clean threshold. Not yet distinguished.

hello6 now does a `glFinish` after **every** upload, in the timed loops and
in the upload+draw+swap pipelines. That is the pattern that passed, and it
is also what a mirroring client would do: one frame's upload in flight.
If the next run still freezes, the next step is a dedicated probe
in this order, least risky first: small sub-uploads to one texture without
`glFinish`, then ping-pong between two textures, then big uploads without
`glFinish`.

Measured before the freeze (RGBA8888, single uploads with `glFinish`): 800×480
in **31 ms ≈ 50 MB/s**, 800×120 in 8 ms. VFB CPU access: store32 9.1 MB/s
(161 ms/frame), memcpy in 9.2 MB/s, memcpy out 4.8 MB/s. Main RAM memcpy
107 MB/s, store32 215 MB/s. RGB565→BGRA on the CPU: 11.9 ms/frame. LZ4
decompress: UI frame 8.8 ms, photo 29 ms.

## 8. 19.10 v2: hello4 survives 8 and 12 tori, freezes on the first 16-tori frame

Run `dev/19.10.2026 - v2`, stress scene only
(`scenes=s step=5 tori=1,2,4,8:60,12,16,24,32,64 trace=1`), after hello5 and
hello6 in the same boot. The HMI underneath showed the **main menu**, not
the nav map (video 2:32). Phone video `VID_20261019_113015_v2.mp4` (kept
outside the repo), hello4 from 2:50.

| tori | frames | avg draw | avg swap | max swap | fps | result |
|---|---|---|---|---|---|---|
| 1 | 133 | 2.0 ms | 22 ms | 52 ms | 26 | ok |
| 2 | 99 | 3.3 | 33 | 51 | 19 | ok |
| 4 | 97 | 7.0 | 31 | 50 | 19 | ok |
| **8** | **899 (60 s)** | 13.2 | 40 | 74 | 14–16 | **ok**: froze after ~25 frames on 11.10 |
| 12 | 64 (5 s) | 20.0 | 44 | 67 | 12–13 | ok |
| 16 | 0 | — | — | — | — | **froze**: last line `stress step 6/9 (16 tori): start` |

- **Nothing built up before the freeze.** Swap time over the 60 s hold
  was flat (first 100 frames 39 ms, last 100 frames 40 ms), free RAM stayed at
  153–154 MB the whole time, and `glError` was 0 every second. Hypothesis 3
  of §4.4 (something leaking in the driver) has no support.
- **The freeze hit the first frame of a new, larger load.** The step line is
  written before the first 16-tori frame. That frame's `f…` line never came
  (each is fsync'ed after its swap). The video holds the last 12-tori frame
  from 4:11 until the camera moves away at 4:33. run.sh's 170 s timeout for
  hello4 never logged, so the whole unit stopped again, as in §4.4.
- `draw=` grows linearly, ~1.65 ms of CPU per torus (client arrays copied
  through the driver). `swap=` (glFinish + vsync wait) is the GPU.

Per-frame client data, from `mesh_draw` (positions + normals as floats,
`GL_UNSIGNED_SHORT` indices; one 561-vertex, 1024-triangle torus drawn n
times): ~19.6 KB per torus, so **12 tori ≈ 235 KB passed, 16 tori ≈ 314 KB
froze on the spot**.

**What 11.10 and 19.10 v2 say together**

| run | screen under it | GL before the stress scene | froze at |
|---|---|---|---|
| 11.10 | nav map | probes + 3 scenes, ~33 s | 8 tori (~157 KB/frame), ~25 frames in, mid-step |
| 19.10 v2 | main menu | none | 16 tori (~314 KB/frame), first frame |

No single per-frame size explains both. A **combined load** does: ours plus
whatever else the GPU is doing (NavCore drawing the map on 11.10, hypothesis
2 of §4.4). Then the system freezes when the total work queued in the driver
passes some limit **[hypothesis]**. 12.10's texture-upload freezes (§6–7) fit
the same picture: they came when several large uploads were queued at once.

**Rules for apps until this is pinned down**

- Keep per-frame client-array geometry **well under ~150 KB** (the 11.10
  freeze level) and the triangle count in the low thousands. GLES-CM 1.0 has
  no VBOs, so every frame re-sends its arrays.
- One large upload in flight at a time (§7).
- The mirroring options in [`phone-mirroring.md`](phone-mirroring.md) need
  neither: option 1 uploads one texture per frame and draws one quad, and
  option 2a doesn't touch GL at all. **hello4 has nothing more to tell the
  mirroring work.** It went back into the default run (last before
  usb_tx_test) with probes, scenes and stress, but the default stress steps
  now stop at 12 tori: 16, 32 and 64 are commented out in `opts_default`.
  After the clean run in §9 it was retired.
  If the threshold ever matters, the cheap follow-up is the 16-tori frame
  split into two halves with a `glFinish` between them: if that survives,
  the limit is per submission, not per frame.

## 9. 21.10: hello4 runs clean end to end, then retires

Run `dev/21.10.2026/`, the full hello4 (`step=10 trace=1`: probes, cube,
light, world, stress 1/2/4/8/12 tori, 10 s each), after hello5 and hello6
(ten video clips through the DSP and VCAP 0) in the same boot. Verdict
`OK`, exit 0, 85 s, no freeze, no swap failures, `glError` 0 throughout,
free RAM flat at 148 MB.

| part | result |
|---|---|
| probes | 9/9 PASS (clear, depth, cull, persp, light, texture, fog, blend, stencil) |
| cube | 229 frames, 22 fps |
| light | 288 frames, 28 fps |
| world | 204 frames, 20 fps |

| tori | frames (10 s) | avg draw | avg swap | max swap | fps |
|---|---|---|---|---|---|
| 1 | 201 | 2.0 ms | 34 ms | 53 ms | 20 |
| 2 | 196 | 3.9 | 33 | 67 | 19 |
| 4 | 178 | 7.0 | 35 | 64 | 17 |
| 8 | 142 | 13.2 | 43 | 75 | 14 |
| 12 | 118 | 20.1 | 51 | 70 | 11 |

- **The user confirms it worked.** 8 and 12 tori passed again, and 8 tori
  passed after the three scenes this time (on 11.10 that combination froze).
  `draw=` per torus is the same as 19.10 v2 (~1.65 ms).
- **Everything is slower than on 11.10 and 19.10 v2.** cube 22 fps (was 57),
  light 28 (52), world 20 (28), and a single torus 20 fps (26 on 19.10 v2).
  The extra time is in `swap=` (glFinish + vsync), not in `draw=`, so the
  GPU or the compositor had more to do. The likely cause is hello6
  running just before: the video displayable and VCAP were disabled and
  unregistered, but io-media/DSP state may linger. The screen under hello4
  may also differ **[unverified]**. Irrelevant for the GPU limits, but
  worth knowing when timing GL after video playback.
- Screenshots `hello4_cube/light/world.png` are again layer-manager
  captures without the GL content (§2).

hello4 has now answered what it was for: the 3D pipeline works, the
per-frame geometry budget is known (§8 rules), and below 16 tori it is
stable over the full sequence. It is out of the Makefile's default
`TESTS`, and its `sd/apps.conf` line is commented out. Run it with
`make sd SD_APPS="hello4 ..."` after uncommenting that line.

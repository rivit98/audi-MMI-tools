# hello4

3D pipeline test on the unit's fixed-function ES-CM 1.0. It starts with
automatic probes (`probe.c`), each read back with `glReadPixels`: clear,
depth, culling, perspective, lighting, texture, fog, blend and stencil.
The results show top right as green/red squares. Then come the scenes:

- **cube:** two cubes
- **light:** a lit gold torus
- **world:** a fogged checkerboard world with crates
- **stress:** 1, 2, 4, 8 and 12 lit tori, measuring triangle rate

- **Run:** `hello4 <sd_path> [options]`, options below. Its `sd/apps.conf`
  line (commented out) is `hello4 <sd> step=10 trace=1`.
- **Output** (in `var/` on the card): `hello4.log` (GL limits, every probe read with its verdict,
  fps and free RAM, triangle-rate table), screenshots of cube/light/world.
- **Status:** Steps of 16 tori and more are
  commented out in `main.c` (`opts_default`). More detail is in
  [docs/gpu-results.md](../../docs/gpu-results.md).

The device's `libGLES_CM.so.6` has no buffer objects (`glBindBuffer` is
not exported), so meshes are client-side arrays drawn with
`glDrawElements`.

**Probes** draw into the back buffer (never swapped) and compare
`glReadPixels` with what the GL spec requires, ±40 per channel for
RGB565. Green square = pass, red = fail, grey = skip.

| Probe | Checks |
|---|---|
| `clear` | clear colour at the centre and both corners (if this fails, the rest is meaningless) |
| `depth` | `GL_LESS` rejects a farther quad drawn later; `glDepthMask(GL_FALSE)` stops depth writes |
| `cull` | CW culled, CCW drawn; `glFrontFace(GL_CW)` reverses it |
| `persp` | `glFrustumf` quads at z = −2 and −4 cover exactly the predicted area (also checks float arguments across the library ABI) |
| `light` | directional diffuse at N·L = 1, 0.707, < 0 gives red 255, 180, black |
| `texture` | all four texels of a 2×2 `GL_NEAREST`, `GL_REPLACE` texture |
| `fog` | linear fog: full, half, none |
| `blend` | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` at α = 0.5 |
| `stencil` | drawing only where a stencil mask was written (`gfx.c` asks for an 8-bit stencil) |

What to look for: in `cube`, flicker or one cube showing through the other
is a depth problem; in `world`, moiré in the far checkerboard means no
mipmapping. The triangle-rate table (`N tori = T tris: frames, ms, fps,
ktris/s`) reads like hello3's fill-rate table; the tori are small, so fill
rate doesn't limit it.

| Option | Meaning | Default |
|---|---|---|
| `<n>` | seconds per scene (cube/light/world), and for the whole stress scene when there is no `step=` | 10 |
| `scenes=pclws` | what runs, in order: `p` probes, `c` cube, `l` light, `w` world, `s` stress | all |
| `tori=1,2,8:60` | stress steps: torus counts, each with optional own seconds | 1,2,4,8,12 |
| `step=<s>` | seconds of a stress step without its own | scene time ÷ steps |
| `trace=0/1/2` | stress only. 1: one fsync'ed line per frame after its swap (`f<frame> n=<tori> draw=<ms> swap=<ms>`); 2: also a line before draw and swap. ~12 ms per line, so fps drops | 0 |

A bad option is a `FAIL` line and exit 2. Each stress step logs its
triangles per frame and free RAM, and the per-second line has `free=<MB>`,
to catch a driver leak. After a freeze, the last `f<n>` line is the last
frame that finished.

## On the car

**Works**
- **All 9 probes pass:** clear, depth, cull, perspective, light, texture,
  fog, blend and stencil.
- **GL limits:**
  - depth 24, stencil 8
  - 8 lights
  - texture 2048
  - modelview stack 32
- **The scenes run:** cube, light and world at 20–57 fps. The full run ends
  with exit 0 and 0 swap failures.
- **Stress up to 12 tori:** 8 tori held for 60 s.

  | tori | 1 | 2 | 4 | 8 | 12 |
  |---|---|---|---|---|---|
  | fps | 20.0 | 19.6 | 17.8 | 14.2 | 11.7 |
  | ktris/s | 20 | 40 | 73 | 116 | 144 |

- **Free RAM stays flat** at about 148–154 MB.

**Doesn't work**
- **16 tori and more freeze the whole unit** on the first 16-tori frame.
  8 tori also froze it once, after the other scenes had run; 12 is the
  largest step that has always passed.
- **The layer manager screenshots** don't show the 3D scenes.

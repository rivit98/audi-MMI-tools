# hello4

3D pipeline test on the unit's fixed-function ES-CM 1.0. It starts with
automatic probes (`probe.c`), each read back with `glReadPixels`: clear,
depth, culling, perspective, lighting, texture, fog, blend and stencil.
The results show top right as green/red squares. Then come the scenes:

- **cube:** two cubes
- **light:** a lit gold torus
- **world:** a fogged checkerboard world with crates
- **stress:** 1, 2, 4, 8 and 12 lit tori, measuring triangle rate

- **Run:** `hello4 <sd_path> [scene_seconds] [scenes=pclws] [tori=...]
  [step=S] [trace=0|1|2]`. The options are explained in `main.c`.
- **Output:** `hello4.log` (GL limits, probe verdicts, fps and free RAM,
  triangle-rate table), screenshots of the scenes.
- **Status:** no longer in the default run. Steps of 16 tori and more are
  commented out in `main.c`. More detail is in
  [docs/gpu-results.md](../../docs/gpu-results.md).

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
- **16 tori and more:** the unit froze on the first 16-tori frame. A heavy
  load froze it at 8 tori once too.
- **The layer manager screenshots** don't show the 3D scenes.

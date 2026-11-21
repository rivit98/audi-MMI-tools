# hello4

3D pipeline test on the fixed-function ES-CM 1.0: automatic probes read
back with `glReadPixels`, then 3D scenes and a triangle-rate stress test.

- **Run:** `hello4 <sd_path> [seconds] [scenes=pclws] [tori=1,2,4,8,12] [step=<s>] [trace=0|1|2]`
- **Output:** `var/hello4.log` (probe verdicts, fps, free RAM, triangle-rate table), screenshots

## Verifies

- Probes: clear, depth, culling, perspective, lighting, texture, fog, blend, stencil.
- Scenes: two cubes, lit torus, fogged world.
- Triangle rate with 1–12 lit tori, and free RAM over time (driver leaks).

## Results

**Works**
- All 9 probes pass.
- Scenes run at 20–57 fps; 8 tori held for 60 s.
- Up to 144 ktris/s (12 tori, 11.7 fps).
- Free RAM stays flat (~150 MB).

**Doesn't work**
- 16 tori or more freeze the whole unit (8 froze it once too); 12 is the
  largest step that always passed.
- No buffer objects (`glBindBuffer` not exported): client-side arrays only.
- Layer manager screenshots don't show the 3D scenes.

Details: [docs/gpu-results.md](../../docs/gpu-results.md).

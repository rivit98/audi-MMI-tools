# hello3

GPU canvas and fill rate. The CPU uploads two small textures once. After
that the GPU produces every pixel. There are three scenes:

1. random-colour blocks that get smaller
2. a three-layer rainbow plasma
3. a fill-rate benchmark: 1 to 32 full-screen blended layers per frame

An FPS counter is drawn top left.

- **Run:** `hello3 <sd_path> [scene_seconds]` (default 10 s per scene)
- **Output:** `hello3.log` (fps per second, per-scene summary, fill-rate
  table), screenshots of scenes 1 and 2.
- **Status:** More detail is in [docs/gpu-results.md](../../docs/gpu-results.md).

How the GPU makes each scene (GLES 1.0 Common only, power-of-two
textures, no per-frame upload):

| Scene | How |
|---|---|
| 1 `pixels` | a 256×256 noise texture with `GL_NEAREST`, so each texel is one block; a random whole-texel jump of the texture matrix gives a new canvas every 100 ms, blocks shrinking 32 → 4 px |
| 2 `plasma` | three layers of a 128×128 tileable plasma texture, each scrolled, rotated and scaled by its own texture matrix, alpha- and additively blended |
| 3 `fill` | N = 1 … 32 full-screen textured, blended layers per frame (no screenshots here, they would skew the timing) |

Reading the fill-rate table (`N layers: frames, ms, fps, Mpixel/s`): fps
near 60 while N grows means fill rate to spare; fps falling as 1/N means
fill-bound, and Mpixel/s is the blended fill rate; low fps even at N=1
means a per-frame cost (`gfx_swap`'s `glFinish` + `eglWaitGL`, or the
layer manager).

## On the car

**Works**
- **Pixels scene:** 57 fps.
- **Plasma scene:** 38 fps (three blended, texture-matrix layers).
- **Whole run:** 42.5 fps average, 0 swap failures, no GL errors.
- **Fill rate** at 800×480, with layers textured and blended:

  | layers | 1 | 2 | 4 | 8 | 16 | 32 |
  |---|---|---|---|---|---|---|
  | fps | 57.4 | 57.5 | 29.6 | 19.5 | 14.8 | 10.7 |
  | Mpix/s | 22 | 44 | 45 | 60 | 91 | 132 |

**Doesn't work**
- **The layer manager screenshots** show the navigation map, not hello3's
  picture.

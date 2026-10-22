# hello2

OpenGL ES check. It brings up ES-CM 1.0 on the screen (`src/common/gfx.c`)
and animates for 20 s: a background cycling through colours, a rotating
square, a bouncing block and a progress bar. It then captures its own
frame three ways (`glReadPixels`, a snapshot of its layer, a snapshot of the
whole display) and hands the screen back.

- **Run:** `hello2 <sd_path> [seconds]`
- **Output:** `hello2.log` (EGL/GL diagnostics, every step),
  `hello2_glread.bmp`, `hello2_dshot.png`, `hello2_sshot.png`.
- **Status:** no longer in the default run. More detail is in
  [docs/gpu-results.md](../../docs/gpu-results.md).

## On the car

**Works**
- **Display:** 800×480 BGRA8888 at 60 Hz with 8 layers; hello2 uses
  layer 7. Layer manager version 1.1.
- **Taking over the screen:** registering displayable 40, its VFB
  (`stride=3200`) and handing the screen back all work.
- **EGL:** 1.3 (NVIDIA), 30 configs. Config 1 gives depth 24 and
  stencil 8.
- **GL:** `NVIDIA EMBEDDED Build 6.0180/NV18`, `OpenGL ES-CM 1.0`.
  `MAX_TEXTURE_SIZE` 2048, 2 texture units, 8 lights.
- **Speed:** the animation runs at 57–59 fps (`avg 58.0 fps`), with 0 swap
  failures and no GL errors.
- **`glReadPixels`** returns the frame we drew, as `hello2_glread.bmp`.

**Doesn't work**
- **The layer manager screenshots:**
  - the layer snapshot (`hello2_dshot.png`) is noise
  - the display snapshot (`hello2_sshot.png`) shows the navigation map,
    not hello2's picture
- **User clip planes:** the driver offers none (`clip planes 0`).

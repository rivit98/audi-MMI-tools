# lmprobe

Layer manager test without OpenGL: replays DrGER's `showScreen` call for
call, but draws eight colour bars straight into the VFB for 5 s and logs
every raw devctl status. Not in the default run (`make sd SD_APPS="lmprobe ..."`).

- **Run:** `lmprobe <sd_path> [seconds]` (default 5)
- **Output:** `var/lmprobe.log`, `lmprobe_dshot.png`, `lmprobe_sshot.png`

## Verifies

- The `showScreen` sequence: unregister → register cid 40 flags 2 → `getVfb`
  → draw → `updateVfb` → show `{40,16,19}` → unregister.
- CPU drawing straight into the VFB.
- Layer manager screenshots of a CPU-drawn VFB.

## Results

**Works**
- Every step `rc=0`; layer manager 1.1; VFB 800×480 `BGRA8888`, stride 3200.
- The bars cover the HMI; the `d-shot` is exact and byte-identical between runs.
- The sequence lives on in `src/common/screen.c` and `src/common/gfx.c`.

**Expected, not errors**
- Unregister before register returns `EINVAL`: check the register `result`, not just the devctl return.

**Not covered**
- The VFB is double-buffered (its `sid` changes on every `updateVfb`);
  lmprobe updates once, so it never sees it.

Details: [docs/platform-notes.md §3](../../docs/platform-notes.md).

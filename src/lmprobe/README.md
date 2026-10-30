# lmprobe

Layer manager test without OpenGL. It replays DrGER's `showScreen` call for
call, but draws eight vertical colour bars (white, yellow, cyan, green,
magenta, red, blue, black) straight into the layer manager's frame buffer
(VFB) instead of a PNG. They replace the picture for 5 s. Every raw devctl
status goes to the log. It was written after the first car run, where the GL
apps' `lm_register_displayable` reported `EINVAL`. It answered that
question, so it is still built and copied to the card, but no longer run by
default.

What it does, in order:

1. **Cleanup:** open `/dev/layermanager`, check the version, unregister
   cid 40, show `{40,16,19}`, close.
2. **Setup:** `gf_dev_attach`, `gf_display_attach` (gives the screen size),
   open, register cid 40 with flags 2, `getVfb`, attach the VFB with
   `gf_surface_attach_by_sid`, fill the bars, `updateVfb`, show
   `{40,16,19}`. Then the layer manager saves a `d-shot` of cid 40 and an
   `s-shot` of display 0 to the card, and the connection is closed.
3. **Wait** the given time. A button press (`/dev/ipc/ch8`), a signal or
   pulling the SD card ends the wait early. A watchdog fires after the run
   time + 30 s.
4. **Cleanup** as in step 1, then free the gf surface, display and device.

- **Run:** `lmprobe <sd_path> [seconds]` (default 5). It isn't in the
  default run. Use `make sd SD_APPS="lmprobe ..."` and uncomment the
  `#lmprobe` line in `sd/apps.conf` (timeout 45 s).
- **Output:** `lmprobe.log`, `lmprobe_dshot.png` (our VFB, written by the
  layer manager) and `lmprobe_sshot.png` (the whole display).
- **Exit code:** `0` if the bars were shown, `1` if setup failed (gf attach,
  register, `getVfb` or surface attach), `2` for bad usage. If the VFB has
  no 32-bit CPU mapping, the bars are skipped and a `FAIL` line is logged,
  but the exit code is still `0`. run.sh then calls the run `WARN`.

## On the car

**10.10.2026, first version run.** lmprobe didn't exist yet. hello2–4
logged `lm_register_displayable(cid=40, 800x480, flags=2): Invalid
argument` and stopped. lmprobe was written to repeat `showScreen`'s
sequence on its own and log each raw status. The cause turned out to be a
GCC 12 `-O2` miscompile of our own result check, not the layer manager
([docs/gcc-sh4-treg-bug.md](../../docs/gcc-sh4-treg-bug.md)).

**10.10.2026, second version run (old toolchain).** Per the docs, lmprobe
ran end to end here too. Its on-car compiler canary read `correct` at the
build's flags and `MISCOMPILED` when forced to `-O2`. This run's files are
no longer in `dev/`, so the numbers below come only from the next run.

**10.10.2026, second version run (new toolchain O2).** `lmprobe OK exit 0,
9 s, 0 FAIL lines`. From `lmprobe.log`:

| step | result |
|---|---|
| compiler canary (GCC 16) | `correct` at the build's flags and at forced `-O2` |
| `lm_check_version` | `rc=0`, version **1.1**, on every connect |
| unregister 40 (before) | `rc=22` (`EINVAL`): nothing was registered, as expected |
| `gf_dev_attach` / `gf_display_attach` | `0` / `0`, **800×480** |
| register cid 40, flags 2 | devctl `rc=0`, `result=1` |
| `getVfb(40)` | `rc=0`, `sid=1`, `uid=4294967295`, `alpha_sid=16` |
| `gf_surface_attach_by_sid(1)` | `0` |
| VFB | 800×480, `format=5152` (`GF_FORMAT_BGRA8888`), stride **3200**, `vaddr=3ebbf000` |
| `updateVfb(40)`, show `{40,16,19}` | `rc=0`, `rc=0` |
| `d-shot`, `s-shot` | both `ok` |
| unregister 40 (after) | `rc=0`: it was still registered, a clean lifecycle |

Timings, from the log's millisecond stamps:

- register at 519 ms, bars filled at 759 ms. The fill took about 180 ms,
  about 8.6 MB/s for the 1.5 MB frame (derived from the stamps, not
  measured on purpose).
- `d-shot` took about 770 ms and `s-shot` about 750 ms.
- the bars were up from the show at 868 ms until the unregister at
  7772 ms, about 6.9 s (5 s wait + 2.4 s of setup and shots; inferred from
  the stamps). The timer stopped the wait. lmprobe exited at 8198 ms.

Screenshots:

- **`lmprobe_dshot.png`** is exactly the eight bars: 100 px wide,
  48,000 pixels each, exact colours, alpha 255. md5 `d7d051ec…`. The docs
  record it as byte-identical between the two runs that day.
- **`lmprobe_sshot.png`** shows the same bars over the whole display, so
  cid 40 covered the HMI. It differs from the d-shot in a single pixel:
  (0,0) is `(206,170,140)` instead of white. The cause is unknown.

The slog has no layermanagerV2 error beyond the expected ones. Each
unregister-before-register logs `removeDisplayable: The displayable 40
couldn't be found`, `unregisterDisplayable: Couldn't remove` and
`ioDevctl: devctl() failed!`. Each show logs `renderLayer: Trying to render
the displayable 40 on a surface whose frame buffer has not been updated
yet` and `processPendingShowRequests: showDisplayable(cid=40,display=0)`.
run.sh's status screens use `showScreen`, which uses cid 40 too, so the
same lines repeat for every screen. lmprobe's own lines can't be picked out
by time stamp.

## What it established

- The reversed `showScreen` sequence works and is deterministic: unregister,
  register cid 40 flags 2, `getVfb`, draw, `updateVfb`, show `{40,16,19}`.
  The CPU can draw straight into the VFB as 32-bit `BGRA8888`, with stride
  = width × 4.
- `EINVAL` from unregister before register is normal. Use `result`
  (`data[4]`) from register, not just the devctl return.
- `d-shot` of a CPU-written VFB is a correct capture.
- The sequence lives on in `src/common/screen.c` (`screen_open`), used by
  hello5, hello6's text screens, hello7, hello8 and usb_tx_test, and in
  `src/common/gfx.c` for the GL apps hello2–4.
- What lmprobe could not show: the VFB is double-buffered and its `sid`
  changes on every `updateVfb`. lmprobe updates only once, so it never saw
  this. `screen.c` fetches the VFB again after each update
  ([docs/platform-notes.md](../../docs/platform-notes.md) §3).

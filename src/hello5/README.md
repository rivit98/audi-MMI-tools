# hello5

Sound from our app. It plays a ping train it generates itself (`pulse.c`:
a 0.5 s ping every 2 s, written to `/tmp`) on io-media's
`mixer:DSPOUT_TDM1_20_SAD`. That is the one output heard in the car so
far. Each step shows on screen what to do, counts down, then shows which
ping is playing:

| step | asks |
|---|---|
| `m` | With the radio on: does the radio keep playing between the pings (mixed) or not (replaced)? |
| `n` | Through hello5's own io-media client (`iom.c`, no `testapp-file`): do pings 4–6 at its own volume 30 sound quieter? Do three queued files keep the 2 s beat? Every io-media event is logged with its time. |
| `v` | Do the pings follow the car's volume knob? Clicks are counted on screen. |
| `t` | After switching to music with MEDIA, does our output still play, and does the music keep playing between the pings? |
| `g` | The output survey: group N = N chimes (`beep.wav`) on each mixer output or io-audio device. |

Steps `m`, `v`, `t` and `g` play through the unit's `testapp-file`.

- **Run:** `hello5 <sd_path> [steps] [outputs]`. The default steps are
  `mnvt`. `outputs` is step `g`'s list (default `m2,m1,m6,m4,m7`).
- **Output:** `hello5.log` (`RESULT` lines; step `n`'s timed events),
  `hello5_<step>.log` (`testapp-file`'s output).
- **Status:** steps `m`, `n`, `v` and `t` have not run on the car yet. The
  results below come from step `g`. More detail is in
  [docs/platform-notes.md](../../docs/platform-notes.md) §9.

The knob/button log that used to be part of hello5 is now
[hello7](../hello7/).

## On the car

**Works**
- **`mixer:DSPOUT_TDM1_20_SAD?delay=130` through io-media is heard
  clearly on the speakers, with the radio on.** It is played by the unit's
  `testapp-file`.
- **These mixer outputs attach and play every clip to `INPUT_EOF`:**
  `TDM1_20_SAD`, `TDM1_51`, `TDM1_20`, `TDM2_20`, `TDM2_20_SAD`,
  `TDM3_20` and `TDM3_20_SAD`. A 2.5 s clip takes 2.46–2.57 s.
- **QSA (`libasound`):** the six io-audio devices (tones1, navi, SDS, BT-Tr,
  NAD, ent3) open, take 44.1 kHz mono S16, and consume each clip in real
  time.
  - `prepare` returns `EBUSY` while the previous clip still plays.
  - Each device N shows up as `hb_fpga: Play buffer underrun strmN` when it
    runs dry.

**Doesn't work**
- **Nothing played through QSA is heard,** on any of the six devices.
- **These mixer outputs play but are silent:** `TDM2_20`, `TDM2_20_SAD`,
  `TDM3_20` and `TDM3_20_SAD`.
- **`TDM1_51` and `TDM1_20` (without the SAD header) mute the car's audio**
  while they play, and their own sound isn't heard.
- **`TDM1_51_SAD` won't attach while the radio plays:** `Mixer entry for
  DSPOUT_TDM1_51_SAD: Already in use`, `iom_errno=0x10001`.
- **`testapp-file`'s exit code is useless:** it is always 1, success
  included.

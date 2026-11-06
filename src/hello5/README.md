# hello5

Sound from our app. It plays a ping train it generates itself (`pulse.c`:
a 0.5 s ping every 2 s, written to `/tmp`) on io-media's
`mixer:DSPOUT_TDM1_20_SAD`, the one output heard in the car. Each step
shows on screen what to do, counts down, then shows which ping is
playing:

| step | asks |
|---|---|
| `m` | With the radio on: does the radio keep playing between the pings (mixed) or not (replaced)? |
| `n` | Through hello5's own io-media client (`iom.c`, no `testapp-file`): are pings 4–6, at its own volume 0, silent? Do three queued files keep the 2 s beat? Every io-media event is logged with its time. |
| `t` | After switching to music with MEDIA, does our output still play, and does the music keep playing between the pings? |
| `v` | Do the pings follow the car's volume knob? Clicks are counted on screen. Last, so a volume left low can't spoil another step. |
| `g` | The output survey: group N = N chimes (`beep.wav`) on each mixer output or io-audio device. |

Steps `m`, `v`, `t` and `g` play through the unit's `testapp-file`.

- **Run:** `hello5 <sd_path> [steps] [outputs]`. The default steps are
  `mntv`. `outputs` is step `g`'s list (default `m2,m1,m6,m4,m7`).
- **Output:** `hello5.log` (`RESULT` lines; step `n`'s timed events),
  `hello5_<step>.log` (`testapp-file`'s output).
- **Status:** all four steps are answered (below): our sound replaces the
  radio and the music while it plays, and follows the car's volume knob.
  hello5's own volume does nothing, and io-media's code says why: the
  `mixer:` output's `set_volume` calls a DSP mixer method that is an empty
  stub (`docs/platform-notes.md` §9, "Why the graph volume does nothing").
  An app that wants to be quieter than the car volume has to scale its own
  samples. The output survey comes from step `g`. More detail is in
  [docs/platform-notes.md](../../docs/platform-notes.md) §9.
- **Screen:** after MEDIA, the MMI's Media screen stays over hello5 (the
  pings and the log go on; only the instructions are hidden), as with
  hello7's MENU. hello5 shows its displayable again every second
  (`screen_set_reshow`, `common/screen.c`). Built, not yet confirmed on
  the car.

The knob/button log that used to be part of hello5 is now
[hello7](../hello7/).

## On the car

Results come from the listener's notes and the sound of phone videos
(the 880 Hz band per ping, 1–5 kHz and below 300 Hz between pings), checked
against hello5's log, ch8 and the system log.

**Works**
- **`mixer:DSPOUT_TDM1_20_SAD?delay=130` through io-media is heard
  clearly on the speakers, with the radio on or Bluetooth music playing.**
  It is played by the unit's `testapp-file` and by hello5's own io-media
  client (`iom.c`). Pings come exactly 2.00 s apart, the first ~0.3 s
  after `testapp-file` starts.
- **It replaces the radio and media audio while it plays, it doesn't mix.**
  - Radio: everything between the pings drops 15–25 dB to the
    recording's floor for the whole train. It stops at the start of the
    file and comes back 0.2–0.3 s after the file ends.
  - Bluetooth music (`t`, after MEDIA): the music plays underneath and is
    muted for the whole train. It was first audible the moment
    `testapp-file` ended. So `TDM1_20_SAD` attaches while the MME plays.
- **Its level follows the car's volume knob.** Pings got ~21 dB quieter
  after a run of detents down and ~12 dB louder after detents up. With no
  knob turned, every ping is as loud as the others (within ~2 dB). No
  dB-per-detent figure: this car's volume knob is slightly faulty and
  sends both directions within single turns (ch8 and the MMI's
  `Encoder Key=0x11` log agree). ch8 byte 5 carries up to 15 detents per
  message on a fast turn, so the screen asks for one click at a time.
- **hello5's own io-media client plays sound:** STATUS "playing" 24 ms
  after `play`, the first TIME event (position 0) at +480 ms, first sound
  ~0.3–0.4 s after `play`.
- **Queueing works, with a gap:** three queued files play, and io-media
  sends **one** `FINISHED` for the whole queue, not one per file. (A
  client that waits for one per file and calls play again replays the last
  file until its time cap.)
- **`testapp-file`:** `TIME` events every ~250 ms, `INPUT_EOF` at 5.1 s of
  a 12 s file, `FINISHED … 0 left to do`. A step takes 12.3–20.7 s for a
  12–20 s train.
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
- **Mixing with the radio or music:** `TDM1_20_SAD` mutes both (above).
- **The graph's volume:** `iom_set_volume` 0 or 30 (devctl `0x80080F22`)
  returns ok and doesn't change the level: pings at volume 0 are as loud
  as the rest (±0.5 dB). io-media's `mixer_set_volume` ends in an empty
  stub; the mixer URL's `volume_ctrl` is about encoder sharing, not volume
  (`docs/platform-notes.md` §9).
- **Gapless queueing:** each queued file adds ~350 ms of silence (queued
  pings come 2.33–2.39 s apart, not 2.0): `INPUT_EOF` → attach next →
  play leaves a gap.
- **Nothing played through QSA is heard,** on any of the six devices.
- **These mixer outputs play but are silent:** `TDM2_20`, `TDM2_20_SAD`,
  `TDM3_20` and `TDM3_20_SAD`.
- **`TDM1_51` and `TDM1_20` (without the SAD header) mute the car's audio**
  while they play, and their own sound isn't heard.
- **`TDM1_51_SAD` won't attach while the radio plays:** `Mixer entry for
  DSPOUT_TDM1_51_SAD: Already in use`, `iom_errno=0x10001`.
- **`testapp-file`'s exit code is useless:** it is always 1, success
  included.

**Not explained**
- In `t` and `v` the **first** ping can come ~12 dB under the others; in
  `m` and `n` it doesn't. Maybe the amplifier fades in when it takes over
  from a media source [inferred].
- Once, in a 24 s queued train, the radio's bass (below ~300 Hz) faded
  back in under the pings from ~10 s on, while everything above ~500 Hz
  stayed silent. It didn't happen again.
- The Bluetooth player sometimes paused around a train: once the phone
  reported `MEDIA_STATE_PAUSED` itself (AVRCP, no `DCMD_MEDIA_PAUSE` from
  the MMI) within a second of the train's start, once the MMI sent
  `DCMD_MEDIA_PAUSE` before a train. Cause unknown; the phone was also in
  use.
- A 29-byte ch8 message with bytes 14–15 `02 02`, then byte 15 `03` and
  `00`, came with a loud click and the radio stopping, and ended a run:
  most likely the car switching off [inferred]
  ([docs/platform-notes.md](../../docs/platform-notes.md) §8).

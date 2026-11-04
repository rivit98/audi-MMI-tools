# hello5

Sound from our app. It plays a ping train it generates itself (`pulse.c`:
a 0.5 s ping every 2 s, written to `/tmp`) on io-media's
`mixer:DSPOUT_TDM1_20_SAD`. That is the one output heard in the car so
far. Each step shows on screen what to do, counts down, then shows which
ping is playing:

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
- **Status:** all four steps are answered (2026-10-30, below): our sound
  replaces the radio and the music while it plays, and follows the car's
  volume knob. 2026-11-03 repeated all four, with the same results.
  hello5's own volume does nothing, and io-media's code says why: the
  `mixer:` output's `set_volume` calls a DSP mixer method that is an empty
  stub (`docs/platform-notes.md` §9, "Why the graph volume does nothing").
  An app that wants to be quieter than the car volume has to scale its own
  samples. The older results come from step `g`. More detail is in
  [docs/platform-notes.md](../../docs/platform-notes.md) §9.
- **Screen:** hello5 shows its displayable again every second
  (`screen_set_reshow`, `common/screen.c`), because after MEDIA the MMI's
  Media screen stayed over it (2026-11-03). Untested on the car.

The knob/button log that used to be part of hello5 is now
[hello7](../hello7/).

## On the car

**2026-11-03, steps `mntv`** (before hello6), radio on, Bluetooth phone
for `t`. Logs in `runs/3.11.2026 - run2/`. Phone videos
`VID_20261103_103843` (`m`, `n`; video ≈ log + 8.3 s, from ping 1 of `n`),
`…104029` (`t` and pings 1–4 of `v`) and `…104111_1` (pings 7–10 of `v`).
The listener left no notes; this comes from the sound (880 Hz band per
ping, 1–5 kHz and <300 Hz between pings) and the system log (slog ≈ log
+ 1229.5 s, from the MEDIA press):

- **`m`, `n`: 30.10 again.** The radio drops to the recording's floor for
  each train and comes back when it ends. In `n`, pings 4–6 at volume 0
  are as loud as the others (fourth run), and the queued pings come
  ~2.35 s apart (~350 ms per queued file).
- **`t`: pings only, no music between them** (the 1–5 kHz band at the
  floor), so `TDM1_20_SAD` replaces Bluetooth music too, as on 30.10.
  This time the music didn't come back after the train. The phone had
  reported `MEDIA_STATE_PAUSED` itself (AVRCP event, no
  `DCMD_MEDIA_PAUSE` from the MMI) within a second of the train's start.
  It played again for ~7 s during `v`'s countdown, then paused with a
  track change, and Bluetooth disconnected right after hello5. The user
  was also using the phone during the run, so these may be the phone
  rather than the MMI.
- **After MEDIA, the MMI's Media screen stayed over hello5's** for the
  rest of hello5 (both videos). The pings and the log went on as normal;
  only the instructions were hidden. Same thing as hello7's MENU/BACK
  (25.10).
- **`v`: every recorded ping is as loud as `t`'s** (5–6 not recorded).
  Nobody turned the knob: ch8 counted 0 clicks and the MMI logged no
  `Encoder` key. An earlier reading of this run said pings 1–4 were
  ~20 dB down, and that was wrong. Measured again (880 Hz band RMS over
  each ping, 820–940 Hz Butterworth), in the same video as `t`:

  | pings | level | between pings |
  |---|---|---|
  | `t` 1–6 (`…104029`) | −44.7 … −46.3 dB | −54 … −60 dB |
  | `v` 1–4 (`…104029`) | −44.2 … −46.2 dB | −59 … −64 dB |
  | `v` 7–10 (`…104111_1`) | −44.0 … −45.0 dB | −54 … −63 dB |

  If the phone's gain control had made up for a 20 dB drop, the level
  between pings would have risen with it. It went down instead, so the
  recording shows no drop. 30.10's knob result stands.
- `testapp-file` exit 1 as always; steps 12.5–20.7 s for 12–20 s trains.

**2026-10-30, steps `mntv`** (after hello8), radio on, Bluetooth phone for
`t`. Logs in `runs/30.10.2026 - media/`, phone video with sound (video time
≈ log time + 22.8 s, from the on-screen clock). The listener's notes
agree with the video's sound (880 Hz band per ping, 1–5 kHz between
pings):

- **`m`: the radio is muted, the pings are heard** (listener). All 6
  pings recorded, 2.00 s apart, the first 0.33 s after `testapp-file`
  started. Between them the 1–5 kHz bands sit 8–10 dB under the radio
  before the step, at the recording's floor. This is the same as on 28.10.
- **`n`: pings heard, no radio** (listener). All 12 recorded.
  - **Pings 4–6 at hello5's volume 0 are as loud as the rest** (±0.3 dB),
    the third run in a row. The graph's volume `devctl` does nothing.
  - Queued pings 10–12 came 2.35, 2.37 and 2.36 s apart, so each queued
    file still costs ~350 ms.
  - 28.10's bass fade-in didn't come back: below 300 Hz stayed at the
    floor for the whole train. From ping 10 on, some unpitched broadband
    sound is in the gaps. It isn't the radio's music, which came back
    only when the queue ended. Probably the cabin [unverified].
- **`t`: pings only, no Bluetooth music** (listener).
  - MEDIA was pressed at 82.2 s. The phone's AVRCP went `PLAYING` at
    ~83.5 s (system log: `DCMD_MEDIA_RESUME`, a track by sanah).
  - All 6 pings recorded, about 7 dB louder than in `m` and `n`. The
    volume knob was also turned during the countdown (MMI's log: net −2),
    so per-source volume or the knob can't be told apart.
  - **Music was first audible at 111.2 s, the moment `testapp-file`
    ended (111.06 s).** So the music plays underneath and is muted for the
    whole train, like the radio. The 15 s between the MEDIA press and the
    train were silent too, for an unknown reason (the song's start?
    Bluetooth start-up?).
  - So `TDM1_20_SAD` attaches while the MME plays, and it **replaces
    media audio as well**.
- **`v`: the pings follow the volume knob** (listener: they got louder
  after turning up). All 10 recorded:
  - pings 2–3 loudest, after 3 detents up
  - pings 4–5 ~21 dB down, after a run of detents down
  - pings 6–10 ~12 dB back up, after detents up

  ch8 counted 18 detents down and 20 up, and the MMI's own log
  (`Encoder Key=0x11`) shows the same mixed directions within single
  turns. This car's volume knob is slightly faulty (the user), which
  explains it. So no dB-per-detent figure can be read from this run. Before the train the MMI paused the Bluetooth player
  (`DCMD_MEDIA_PAUSE` at ~120 s, cause unknown). Car audio was back
  ~0.3 s after the train.
- In `t` and `v` the **first** ping came ~12 dB under the others. In `m`
  and `n` it didn't. Maybe the amplifier fades in when it takes over from
  a media source [inferred].
- `testapp-file` exit 1 as always. The steps ran 12.3–20.7 s for 12–20 s
  trains.

**2026-10-28, steps `mntv`** (after hello8), radio on, music audible in
the phone video this time (video time ≈ log time + 8.5 s, from the
on-screen clock). Logs in
`runs/28.10.2026 - c++, sound/`. Results below come from the video's
sound: 880 Hz band per ping, and the rest of the spectrum between
pings.

- **`m`: the radio is replaced, not mixed.** All 6 pings were recorded,
  exactly 2.00 s apart. The radio, music with a bass beat before and
  after the step, **is gone for the whole 12 s train**: everything
  between the pings drops 15–25 dB to the recording's noise floor. It
  stops at the start of the file and comes back ~0.2 s after the file
  ends. So `TDM1_20_SAD` doesn't mix with the radio, it takes the TDM1
  stream over while it plays (as `TDM1_20` and `TDM1_51` did, but these
  are heard).
- **`n`: our own volume does nothing.** All 12 pings were recorded.
  `volume 0` before ping 4 and `volume 100` before ping 7 both returned
  ok, but **pings 4–6 are exactly as loud as the others** (peaks within
  ±0.5 dB over all 18 pings of `m` and `n`). The same is true of 25.10's
  volume 30. So the volume `devctl` (`0x80080F22`) is accepted and has
  no effect on this output. A candidate to try is the mixer URL's own
  `volume_ctrl` key [unverified].
  - Queued files again add a gap: pings 10–12 came 2.38, 2.39 and 2.33 s
    apart, so each `INPUT_EOF` → attach → play costs **~350 ms** (25.10:
    the same).
  - One `FINISHED` for the queue, as expected; the step ended "played to
    the end" after 25.6 s (24 s of pings).
  - The radio was muted here too, but **from ~10 s into the train its bass
    (below ~300 Hz, beat included) faded back in** over ~8 s, under the
    pings. Everything above ~500 Hz stayed silent until the queue
    finished. `m`'s 12 s train didn't show this. Not explained.
  - First sound ~0.3–0.4 s after `play` (±0.2 s, the frame alignment).
- **`t` never played.** During its 25 s countdown no MEDIA press reached
  ch8, and the screen still showed hello5 at 72 s. At **84.7 s** (video
  93.2 s) ch8 sent a 29-byte message with bytes 14–15 `02 02`. In the same
  instant the video has a loud click, the radio stops, and a tone falls
  from ~1 kHz to ~700 Hz over 2.5 s, like a motor running down. Byte 15
  then went `03` (85.3 s) and `00` (88.3 s). The log ends there,
  `run.log` ends at `--- hello5`, and hello6 never ran. Byte 15 was `02`
  in all 282 earlier ch8 messages, so it looks like the unit's power state,
  and the run ended with the car switching off [inferred] (see
  platform-notes §8).
- `testapp-file` (step `m`): `TIME` events every ~250 ms, `INPUT_EOF`
  at 5.1 s of the 12 s file, `FINISHED … 0 left to do`, exit 1 as always.

**2026-10-25, steps `mnvt`** (then the default), radio on. The listener
heard pings only in step `m`. The phone video's sound says more: below,
"recorded" means the 880 Hz ping stands out of the cabin noise in the
video, at its time on screen.

- **`m`: all 6 pings recorded**, ~15 dB over the noise. `testapp-file`
  played the 12 s train from `/tmp` in 12.3 s. Whether the radio kept
  playing between them can't be told from the video: the radio isn't
  audible in it at all.
- **`n`: all 12 pings recorded, as loud as `m`'s. Our own io-media client
  plays sound.** STATUS "playing" came 24 ms after `play`, the first TIME
  event (position 0) at +480 ms.
  - Volume 30 for pings 4–6: at most ~3 dB down, and pings 7–9 (back at
    100) were no louder. Not conclusive, so the low volume is 0 now.
  - The three queued files played, but **each added ~350 ms of silence**
    (pings 10–12 came 2.35 s apart, not 2.0): `INPUT_EOF` → attach next →
    play leaves a gap. No gapless queueing this way.
  - io-media sends **one** `FINISHED` for the whole queue. hello5 expected
    one per file and called play again on it, which replayed the last file
    until the time cap ("NOT played to the end"). Fixed.
- **`v`: only ping 1 recorded.** The knob was turned right after it, fast:
  148 clicks down and 37 up counted (ch8 byte 5 carries up to 15 detents
  per message). Pings 2–10 are not in the video. So the pings follow the
  car volume, or the volume simply ended near 0. The screen now asks for
  one click at a time.
- **`t`: nothing recorded**, and no music audible either: most likely the
  volume was still down from `v`. So `v` now runs last.
- `testapp-file` exits 1 every time, as before.

**Works**
- **`mixer:DSPOUT_TDM1_20_SAD?delay=130` through io-media is heard
  clearly on the speakers, with the radio on or Bluetooth music playing.**
  It is played by the unit's `testapp-file` and by hello5's own io-media
  client (`iom.c`). **It replaces the radio and media audio while it
  plays** (2026-10-28, 2026-10-30). The car audio comes back 0.2–0.3 s
  after the stream ends.
- **Its level follows the car's volume knob** (2026-10-30).
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
- **The graph's volume:** `iom_set_volume` 0 or 30 returns ok and doesn't
  change the level (2026-10-25, 2026-10-28, 2026-10-30, 2026-11-03).
  io-media's `mixer_set_volume` ends in an empty stub; the mixer URL's
  `volume_ctrl` is about encoder sharing, not volume
  (`docs/platform-notes.md` §9).
- **Gapless queueing:** each queued file adds ~350 ms of silence.
- **Nothing played through QSA is heard,** on any of the six devices.
- **These mixer outputs play but are silent:** `TDM2_20`, `TDM2_20_SAD`,
  `TDM3_20` and `TDM3_20_SAD`.
- **`TDM1_51` and `TDM1_20` (without the SAD header) mute the car's audio**
  while they play, and their own sound isn't heard.
- **`TDM1_51_SAD` won't attach while the radio plays:** `Mixer entry for
  DSPOUT_TDM1_51_SAD: Already in use`, `iom_errno=0x10001`.
- **`testapp-file`'s exit code is useless:** it is always 1, success
  included.

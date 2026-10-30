# hello7

Knob and button tester. It decodes every `/dev/ipc/ch8` message
(`keys.c`) and shows it live:

- a counter per control: main knob right/left, volume up/down, MENU,
  top-left soft key, buttons `b1 0x40` and `b1 0x80`, unknown
- the latest messages with their first 16 bytes

Any bit no known control explains shows as `unknown b<byte> 0x<bits>`. To
name a new button, press it on its own.

It is a tool, not a test, so it isn't in the default run. It was in the
default run once (2026-10-25) and taken out again: the MMI handles every
button too, and one press of MENU switched the screen to the MMI's own
menus for the rest of hello7's run.

- **Run:** `hello7 <sd_path> [seconds]` (default 60), or
  `make sd SD_APPS="hello7 ..."`.
- **Output:** `hello7.log`: every message in full (27 bytes) with its
  decoding and the time since the previous one, then `RESULT` counts.
- **Status:** ran once, 2026-10-25 (below). The older facts come from the
  same ch8 log in hello5.

## On the car

**2026-10-25, first run (60 s, 44 messages):**
- Decoded right: main knob right/left (1 each), volume up 2 / down 4,
  MENU (byte 3 `0x04`), top-left soft key (byte 1 `0x02`), `b1 0x40` (5),
  `b1 0x80` (1).
- New bits, one press each: byte 1 `0x04`, `0x10`, `0x20`; byte 0 `0x10`,
  `0x08`, `0x01`, `0x02`, `0x40`. Nobody wrote down which button was
  which, so they are still unnamed.
- Three longer messages (29 and 33 bytes) with byte 14 = `02` instead of
  `01`, one right after a button release.
- **Every** message had byte 16 = `0x03` (it was `0x00` on 21.10) and
  byte 26 = `0x11`, in hello5's log of the same day too, where bytes 23
  and 25 also changed with no control touched. hello7 showed them as
  "unknown" bits, so the 112 "unknown" presses are mostly those. Bytes
  16–26 look like state of the unit, not controls [inferred]: `keys.c` no
  longer decodes them (they stay in the raw log).
- **The MMI took the screen back:** after the MENU press at +18.7 s the
  MMI's main menu came up over hello7 (phone video), and the following
  presses moved through the MMI's own screens (Setup MMI, radio, Audi
  drive select) until hello7 ended. hello7 kept logging the whole time.

**Works**
- **Reading `/dev/ipc/ch8`:** messages are 27 bytes, and bytes 14–15 are
  always `01 02`.
- **Knobs:**
  - Byte 6/7 is the main knob right/left.
  - Byte 4/5 is the volume knob (+/−).
  - The value is the number of detents: a fast turn sends `02`.
  - MMI3GApplication's own log shows every turn the same way:
    `[KEY] Encoder Key=0x10` for the main knob, `0x11` for volume, with
    `Incr=±n`.
- **Buttons:** a button is a bit in byte 1 or 3 while pressed, then an
  all-zero message on release, 80–300 ms later. These bits have been seen:
  byte 3 `0x04`, byte 1 `0x02`, byte 1 `0x40` and byte 1 `0x80`.

**Doesn't work**
- **Keeping input from the HMI:** the MMI still reacts to every knob and
  button while hello7 is on screen.
- **Button names:** none is identified yet. "MENU" (byte 3 `0x04`) and
  "top-left soft key" (byte 1 `0x02`) are guesses, and `0x40` and `0x80`
  are unknown.
- **Volume turns are not always clean:** the volume knob sometimes sends
  messages for both directions in one turn. The HMI sees the same.

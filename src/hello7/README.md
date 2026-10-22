# hello7

Knob and button tester. It decodes every `/dev/ipc/ch8` message
(`keys.c`) and shows it live:

- a counter per control: main knob right/left, volume up/down, MENU,
  top-left soft key, buttons `b1 0x40` and `b1 0x80`, unknown
- the latest messages with their first 16 bytes

Any bit no known control explains shows as `unknown b<byte> 0x<bits>`. To
name a new button, press it on its own.

It is a tool, not a test, so it isn't in the default run.

- **Run:** `hello7 <sd_path> [seconds]` (default 60), or
  `make sd SD_APPS="hello7 ..."`.
- **Output:** `hello7.log`: every message in full (27 bytes) with its
  decoding and the time since the previous one, then `RESULT` counts.
- **Status:** hello7 itself has not run on the car yet. The facts below
  come from the same ch8 log in hello5.

## On the car

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

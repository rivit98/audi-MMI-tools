# hello7

Knob and button tester. It decodes every `/dev/ipc/ch8` message
(`keys.c`) and shows it live:

- a counter per control: main knob right/left, volume up/down, MENU,
  top-left soft key, buttons `b1 0x40` and `b1 0x80`, unknown
- the latest messages with their first 16 bytes

Any bit no known control explains shows as `unknown b<byte> 0x<bits>`. To
name a new button, press it on its own.

It is a tool, not a test, so it isn't in the default run: the MMI handles
every button too, and one press of MENU switches the screen to the MMI's
own menus for the rest of hello7's run.

- **Run:** `hello7 <sd_path> [seconds]` (default 60), or
  `make sd SD_APPS="hello7 ..."`.
- **Output:** `hello7.log`: every message in full (27 bytes) with its
  decoding and the time since the previous one, then `RESULT` counts.
- **Status:** ran on the car once (60 s, 44 messages). Some facts below
  come from the same ch8 log in hello5.

## On the car

**Works**
- **Reading `/dev/ipc/ch8`:** messages are 27 bytes, and bytes 14–15 are
  usually `01 02`. Longer messages (29 and 33 bytes) come with byte 14 =
  `02` instead of `01`, one seen right after a button release.
- **Knobs:**
  - Byte 6/7 is the main knob right/left.
  - Byte 4/5 is the volume knob (+/−).
  - The value is the number of detents: a fast turn sends `02`.
  - MMI3GApplication's own log shows every turn the same way:
    `[KEY] Encoder Key=0x10` for the main knob, `0x11` for volume, with
    `Incr=±n`.
- **Buttons:** a button is a bit in byte 0, 1 or 3 while pressed, then an
  all-zero message on release, 80–300 ms later. These bits have been seen:
  - byte 3 `0x04` ("MENU"), byte 1 `0x02` ("top-left soft key"), byte 1
    `0x40` and `0x80`
  - one press each, unnamed: byte 1 `0x04`, `0x10`, `0x20`; byte 0
    `0x10`, `0x08`, `0x01`, `0x02`, `0x40`
- **Bytes 16–26 look like state of the unit, not controls** [inferred]:
  byte 16 has been seen as `0x00` and as `0x03`, byte 26 as `0x11`, and
  bytes 23 and 25 change with no control touched. `keys.c` doesn't decode
  them (they stay in the raw log).

**Doesn't work**
- **Keeping input from the HMI:** the MMI still reacts to every knob and
  button while hello7 is on screen. After a MENU press the MMI's main menu
  comes up over hello7, and further presses move through the MMI's own
  screens (Setup MMI, radio, Audi drive select). hello7 keeps logging.
- **Button names:** none is identified. "MENU" (byte 3 `0x04`) and
  "top-left soft key" (byte 1 `0x02`) are guesses, and the other bits are
  unknown.
- **Volume turns are not always clean:** the volume knob sometimes sends
  messages for both directions in one turn. The HMI sees the same. This
  car's volume knob is slightly faulty, so that is the hardware, not the
  decoding.

# hello7

Knob and button tester: decodes every `/dev/ipc/ch8` message (`keys.c`),
shows it and logs it. A tool, not in the default run.

- **Run:** `hello7 <sd_path>` (guided, about 3 minutes) or
  `hello7 <sd_path> <seconds>` (free mode)
- **Output:** `var/hello7.log`

## Guided mode (default)

The screen names **one control at a time** (`guide.c`): main knob right /
left / press, the four joystick directions, the four soft keys, BACK, MENU,
NAV, TEL, RADIO, MEDIA, CAR, volume up / down / press twice (mute, unmute),
the skip keys, the touch pad and two steering-wheel keys. 27 steps.

- Press or turn the named control, then wait. The step moves on **2 s after
  the last message**, so a knob can take several clicks.
- A control the car doesn't have: touch nothing, the step moves on after
  **15 s** ("nothing came"). No step lasts more than 30 s.
- While a step is up the screen shows what arrived so far ("Got: b6 +3"),
  the latest raw messages and a countdown.

Every step's **signature** is what arrived during it: the detents per knob
byte (bytes 4–7, `b6 +3`) and the bits seen in every other control byte
(`b1 0x02`). The unit-state bytes 16–26 are left out (`keys.c`).

The log has, per step: the prompt (`== step 5/27: ...`), every message with
the step it belongs to (`key #12 (+840 ms) [5 joystick down]: ...`), and the
result (`step 5 joystick down: b9 0x10; 2 messages, 1 with a control`).
`RESULT` lines at the end list all steps. **`SAME as <step>`** marks a step
that got the same bytes as an earlier one: the same control under two names,
or a step where the wrong control was used. The last screen shows the same
table for 45 s.

## Free mode

`hello7 <sd> 60`: no prompts. A counter per known control (the last one used
in yellow) and the latest messages with their first 16 bytes; a bit no known
control explains shows as `unknown b<byte> 0x<bits>`.

## Both modes

- The HMI still acts on every knob and button. MENU, BACK and the source
  keys bring its screens up; hello7 shows its own again every second.
- While the HMI shows the rear camera, hello7 draws nothing and doesn't
  re-show (`cam.cpp` over `common/camera`); it comes back after the camera.

## Results

**Works**
- Main knob (bytes 6/7) and volume knob (bytes 4/5) decode, with detent counts.
- Buttons show as a bit in byte 0, 1, 2 or 3, then an all-zero release message.
- The guided mode ran end to end (27 steps, ~3 min): 21 steps got their
  control, no two steps the same bytes. Every button is now named
  (platform-notes §8 table).
- The touch pad sends 29/30-byte messages (bytes 14–15 `02 02`) with extra
  bytes after byte 26 (`ca ..` down, `3d`/`3e` + coordinate, `cb` up).

**Doesn't work**
- The HMI still acts on every knob and button.
- Joystick (main knob tilt, 4 steps) and the steering-wheel keys (2 steps):
  nothing on ch8.
- `keys.c` still names only the old eight controls; the rest log as
  `unknown b<byte> 0x<bits>`. The touch pad's coordinates are not decoded.
- This car's volume knob sometimes reports both directions in one turn (hardware fault).

Details: [docs/platform-notes.md §8](../../docs/platform-notes.md).

# hello7

ch8 input tester: decodes every `/dev/ipc/ch8` message (panel knobs and
buttons in `keys.c`, steering wheel and touch pad in `tpad.c`), shows it
and logs it. A tool, not in the default run.

- **Run:** `hello7 <sd_path>` (guided, 19 steps, ~5–11 minutes) or
  `hello7 <sd_path> <seconds>` (free mode)
- **Output:** `var/hello7.log`

## Guided mode (default)

The screen asks **one open question at a time** (`guide.c`). The panel's
knobs and buttons are all named already, so only these are left:

| Steps | What to do | What answers it |
|---|---|---|
| wheel volume up / down | roll the steering wheel's volume roller 3 clicks each way | `wheel 12 +3` / `wheel 12 -3`: which code is the volume roller, and which sign is louder |
| pad tap, double tap | tap the pad in the middle | `pad ca cb` (+ `d3` if this mode reports gestures) |
| pad corners | touch the four corners in turn | the `CA` finger-down positions: the pad's x/y range and orientation (`down at` in the log) |
| pad line right / down | one slow line across | which subfunctions a stroke gives in the current mode |
| pad mode 6 .. 18 | hello7 sets each recognizer mode, then: line, letter A, tap | the pad's `mode N` answer (the write works) and what each mode reports: keys, scroll, slider, coordinates, characters |
| radio / map / speller screen | open that HMI screen, then use the pad there | the mode number the HMI picks for each screen (`mode N`), and what it gives |

- A step moves on **3 s after the last pad touch** (2 s on the wheel, 4 s on
  HMI screens), or after **15 s** with nothing (40 s on the radio and map
  screens, 60 s for the speller). Each step counts only what it asks for:
  a wheel step ignores the knob, a pad step ignores buttons and the pad's
  own mode answers.
- **The mode steps write to ch8**, the same 10-byte command the HMI sends
  when it changes screen (`22 00 00 00 00 00 02 00 C4 <mode>`; protocol in
  [src/common/buttons/README.md](../common/buttons/README.md)). Nothing is
  stored: the HMI sets its own mode again on its next screen change, and
  the three HMI-screen steps come last for that reason. If the run stops
  during the mode steps, hello7 sets the mode the pad had before (when it
  answered one) or logs that it was left.
- **HMI-screen steps** let the HMI stay on top (no re-show); hello7 comes
  back by itself when the step ends.
- While a step is up the screen shows what arrived so far ("Got: pad ca cb,
  mode 6"), the latest decoded messages and a countdown.

The log has, per step: the prompt (`== step 5/19: ...`), `pad mode N: sent
...` for the mode steps, every message decoded with its step
(`key #12 (+840 ms) [8 pad mode 6]: pad key approached KEY6; 29 bytes:
...`), and the result (`step 8 pad mode 6: pad 3d 3e c4 ca cb, mode 6; 9
messages, 7 it wants, 4210 ms; finger down at 132,198`). `RESULT` lines at
the end list all steps. **`SAME as <step>`** marks a step that got the same
codes and subfunctions as an earlier one (the mode aside): two modes that
report the same way. The last screen shows the same table for 45 s.

## Free mode

`hello7 <sd> 60`: no prompts. A counter per known panel control (the last
one used in yellow) and the latest messages, decoded: panel bits, `wheel
roller 0x12 +1`, `pad press x=132 y=198`, `pad mode 6`, ... A panel bit
no known control explains shows as `unknown b<byte> 0x<bits>`.

## Both modes

- The HMI still acts on every knob, button, wheel key and pad stroke. MENU,
  BACK and the source keys bring its screens up; hello7 shows its own again
  every second (not during the HMI-screen steps).
- While the HMI shows the rear camera, hello7 draws nothing and doesn't
  re-show (`cam.cpp` over `common/camera`); it comes back after the camera.

## Results

**Works**
- Main knob (bytes 6/7) and volume knob (bytes 4/5) decode, with detent counts.
- Buttons show as a bit in byte 0, 1, 2 or 3, then an all-zero release message.
- An earlier guide (27 steps) named every panel button
  ([src/common/buttons/README.md](../common/buttons/README.md) table).
- The touch pad's radio-screen stroke decodes: `CA` down at x, y, `3D`/`3E`
  over the preset keys, `CB` up.

**Doesn't work / open**
- The HMI still acts on every input.
- Joystick (main knob tilt): nothing on ch8; this car has none.
- The steering wheel steps of the earlier guide got no ch8 message, while
  other runs logged roller `12` messages on ch8. The new wheel steps say
  which.
- The new guide (wheel + touch pad, pad mode writes) is built and
  host-tested, not yet run on the car.
- `keys.c` names only the old eight panel controls; the full table is in
  [src/common/buttons](../common/buttons/README.md).
- This car's volume knob sometimes reports both directions in one turn (hardware fault).

Details: [src/common/buttons/README.md](../common/buttons/README.md), the
protocol and the library that decodes ch8 for other apps.

# hello7

Knob and button tester: decodes every `/dev/ipc/ch8` message (`keys.c`) and
shows per-control counters and the latest raw bytes. A tool, not in the
default run.

- **Run:** `hello7 <sd_path> [seconds]` (default 60)
- **Output:** `var/hello7.log` (every message, decoded, with timing)

## Verifies

- The ch8 message format: knobs, buttons, and unknown bits.
- Whether an app can keep input from the HMI.

## Results

**Works**
- Main knob (bytes 6/7) and volume knob (bytes 4/5) decode, with detent counts.
- Buttons show as a bit in byte 0, 1 or 3, then an all-zero release message.

**Doesn't work**
- The HMI still acts on every knob and button; MENU brings its menu over the app.
- Button names are guesses ("MENU" = byte 3 `0x04`); the rest are unnamed.
- This car's volume knob sometimes reports both directions in one turn (hardware fault).

Details: [docs/platform-notes.md §8](../../docs/platform-notes.md).

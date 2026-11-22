# buttons

A C++ library: the unit's knobs and buttons as events, for any app. It
decodes `/dev/ipc/ch8` (the protocol below) and hands the events to any
number of observers. Not an app: link it into one (the Makefile's
`BUTTONS` group, `-Isrc/common/buttons`). The runner ([src/runner](../../runner/README.md))
uses it.

- `buttons.h` / `.cpp`: `Decoder`, pure. One ch8 message in, events out:
  **Press** / **Release** per button bit (against the held state),
  **Turn** per knob byte (signed detents), **Touch** for the touch pad's
  messages (raw bytes), **Power** when byte 15 changes.
- `hub.h` / `.cpp`: `Hub` (observer: `subscribe(callback)` /
  `unsubscribe(id)`) and `EventQueue` (a subscriber that queues events for a
  main loop: `wait(e, ms)`, `poll(e)`, `clear()`).
- Tests: [tests/test_buttons.cpp](../../../tests/test_buttons.cpp), on messages
  logged on the car.

## Using it

The process reads ch8 once. `quit.c` already reads it on its key thread in
every app and never stops (an unread channel stalls every reader, see
"Several readers" below), so the hub is fed from there:

```cpp
buttons::Hub &hub = *new buttons::Hub;      // lives until exit: quit.c's thread calls it
quit_init_ex(sd_path, watchdog_s, buttons::Hub::quit_cb, &hub);

buttons::EventQueue q(hub);                 // for a main loop
buttons::Event e;
if (q.wait(e, 100) && buttons::is_knob_press(e)) { ... }

int id = hub.subscribe([](const buttons::Event &e) { ... });   // or a callback
```

- Callbacks run on quit.c's key thread with the hub's lock held: short, no
  blocking, no `subscribe` / `unsubscribe` from inside one.
- `unsubscribe` returns once no callback of that id is running.
- `EventQueue` drops the oldest event when full (`dropped()`), and
  `clear()` forgets what came while the app was busy.
- `describe(e)` gives a log line: `main knob turn +2`, `BACK press`,
  `unknown b9 0x10 press`, `touch pad ca 84 c6`, `power 03`.

| Event | Key | Fields |
|---|---|---|
| Turn | `MainKnob` (+ clockwise), `VolumeKnob` (+ up) | `detents` |
| Press / Release | every button in the table, `MainKnob` / `VolumeKnob` for their press, `Unknown` for an unnamed bit | `byte`, `mask` |
| Touch | `TouchPad` | `touch[]`, `touch_len`: the bytes after byte 26 |
| Power | `None` | `power`: the new byte 15 |

## The protocol: `/dev/ipc/ch8`

The unit's only user input reaches an app as 27-byte messages on
`/dev/ipc/ch8`. Each message is the current state of the controls: bytes
14–15 are normally `01 02` (byte 17 = `01` too), and a message with all
control bytes zero follows a button release. Named one control at a time
by hello7's guided mode (A6 C7, MMI 3G+ with touch pad) [confirmed]:

| byte | value | control |
|---|---|---|
| 6 | detents | main rotary knob **clockwise (right)** |
| 7 | detents | main rotary knob **anticlockwise (left)** |
| 4 | detents | volume knob **up** |
| 5 | detents | volume knob **down** |
| 0 | `0x01` | **RADIO** |
| 0 | `0x02` | **MEDIA** |
| 0 | `0x08` | **TEL** |
| 0 | `0x10` | **NAV/MAP** |
| 0 | `0x40` | **CAR** |
| 1 | `0x01` | **next track** (`>>\|`) |
| 1 | `0x02` | soft key **top left** |
| 1 | `0x04` | soft key **bottom left** |
| 1 | `0x08` | **previous track** (`\|<<`) |
| 1 | `0x10` | soft key **top right** |
| 1 | `0x20` | soft key **bottom right** |
| 1 | `0x40` | **BACK** |
| 1 | `0x80` | main knob **press** |
| 2 | `0x01` | volume knob **press** (mute and unmute alike) |
| 3 | `0x04` | **MENU** (same second as the HMI's `OnOff new system state is 48`) |

Not on ch8 at all: the main knob's four tilt directions (a joystick; none
came in 15 s each, so this car's knob has none or they travel elsewhere)
and the steering-wheel volume wheel and arrow keys (most likely the wheel
reaches the unit as BAP/CAN on another channel [inferred]).

```
00 00 00 00 00 00 01 00 00 00 00 00 00 00 01 02 00 01 ...  main knob 1 detent right
00 00 00 00 00 02 00 00 00 00 00 00 00 00 01 02 00 01 ...  volume 2 detents down (fast turn)
00 00 00 04 00 00 00 00 00 00 00 00 00 00 01 02 00 01 ...  MENU down
00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 02 00 01 ...  button released
```

- The knob bytes are **counts**: a quick turn sends one message with `02`.
  Main knob 5 + 5 clicks gave exactly 5 + 5 messages. Volume 3 up gave 3,
  and "3 down" gave 4 messages carrying 5 detents (`01 01 01 02`), so the
  knob reports what it physically moved [confirmed].
- **This car's volume knob is slightly faulty.** Its turns often come with
  detents in the other direction mixed in (`Incr=1` among `Incr=-1`s, `-6`
  in a slow turn up), in ch8 and in the MMI's own `Encoder Key=0x11` log
  alike. That is the knob, not the protocol. The main knob's counts are
  clean [confirmed]. The decoder reports both bytes of such a message as
  two Turn events.
- MMI3GApplication logs the same turns in the system log, one line per ch8
  message at the same moments: `[KEY] Encoder Key=0x10 Incr=±n` for the
  main knob (byte 6 = +n, byte 7 = −n) and `Encoder Key=0x11` for the
  volume knob (byte 4 = +n, byte 5 = −n), sizes included (`02` →
  `Incr=-2`) [confirmed, several runs].
- Buttons are bitmasks in bytes 0–3, sent on press, then an all-zero
  message on release, ~70–350 ms later [confirmed]. Every message carries
  the whole state, so the decoder compares each message's bits with the
  previous one: a new bit is a Press, a cleared bit a Release.
- Bytes 16–26 are **state, not controls** [inferred]: byte 16 is constant
  within a run but has been seen as `00` in one and `03` in another,
  byte 26 as `11`, and bytes 23/25 change with no control touched. The
  decoder ignores them.

### Touch pad: the 29/30-byte messages

Touch pad strokes come as messages with bytes 14–15 = `02 02` and 1–3
extra bytes after byte 26 [confirmed]:

```
... 02 02 00 01 ... 00  ca 84 c6     finger down (first byte 0xCx)
... 02 02 00 01 ... 00  3d 3c        moves: 0x3d/0x3e + one coordinate byte
... 02 02 00 01 ... 00  3e 3a
... 02 02 00 01 ... 00  cb           finger up
```

The same `02 02` + `c4 xx` pair also arrives ~0.6 s after a main-knob
press or a soft key (`c4 06`, `c4 09`), with no hand on the pad. The pad's
coordinate encoding (and what `c4 xx` is) is not decoded yet; the first
extra byte looks like a type (`0xC4`/`0xCA`/`0xCB` events, `0x3D`/`0x3E`
axis samples) [inferred]. Their control bytes are zero, so the decoder
takes a `02` message as a Touch event only: it releases no held button.

### Byte 15: the unit's power state

Byte 15 looks like the unit's power state [inferred]. It is `02` in normal
operation. With nothing touched, ch8 sends a 29-byte message with bytes
14–15 = `02 02`, then `01 03` 0.2–0.6 s later and `01 00` about 3 s after
that. This sequence has been seen twice:

- Once the run ended there: the app's log, `run.log` and the next app all
  stopped. At the first message there was a loud click, the radio stopped,
  and a tone fell from ~1 kHz to ~700 Hz over 2.5 s, like a motor running
  down. The car most likely switched off (ignition off or the unit going
  to standby), and the SD writes stopped with it (or the card was pulled).
- Once the unit kept running: the app played on for 3 more minutes and
  run.sh logged its result. MMI3GApplication logged `OnOff new system
  state is 5` in the same second as the sequence, `7` 18 s earlier, and
  `19` with `AMFMTuner disconnected` 2 min later. The states seen while
  running normally are `14`, after `49 20 35 4 6` at boot.

So byte 15 `03`/`00` is a power state on the way down, with the unit
possibly still running for minutes [inferred]. If that holds, ch8 gives an
app a few seconds' warning to save its state (the decoder's Power event).
To confirm: switch the ignition off on purpose during a run and compare.

### Several readers

dev-ipc gives every reader of a channel its own copy of each message, and
a reader that keeps the channel open without reading stalls it for every
reader after 32 messages ([docs/vehicle-data.md §2.2](../../../docs/vehicle-data.md)).
For ch8 that includes MMI3GApplication, so: one reader per process
(quit.c's), always reading, and no ch8 fd inherited by a child that won't
read it (the runner spawns its apps with only stdin/stdout/stderr).

### The HMI keeps the input

The HMI still acts on every knob and button while an app is on screen
[confirmed]. Keeping input away from the HMI is open (`docs/TODO.md`). A
MENU press brings the HMI's main menu up over the app's displayable, and
it stays on top: the HMI re-shows its own screens when it handles a
button, and the app's layer doesn't come back by itself (apps re-show on a
timer, `screen_set_reshow`). Design apps around the main knob; avoid MENU,
BACK and the source keys.

**No key or button stops an app.** Runs end on their own timer, a signal,
SD removal or the watchdog, so a stray press during a test cannot cut it
short. `quit.c` reads ch8, logs the first messages raw and offers the
bytes to an app observer (`quit_init_ex`): the hub, or a plain callback
(hello5 counts volume detents that way). **hello7** is the input tester:
its guided mode named the table above, one control per step.

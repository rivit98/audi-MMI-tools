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
quit_init(sd_path, watchdog_s, buttons::Hub::quit_cb, &hub);

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
- The hub logs every event with the subscribers it went to
  (`buttons: BACK press -> runner, #2`; `-> nobody` without any), every
  subscribe / unsubscribe, and an `EventQueue`'s drops (at 1, 2, 4, 8, ...).
  Name a subscriber with `subscribe(cb, "name")` / `EventQueue(hub, cap,
  "name")` (a literal; unnamed ones log as `#<id>`). Lines are written after
  the hub's lock is released, so subscribers don't wait for the SD card.
  `set_logging(false)` silences it.
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

Not on this car: the main knob's four tilt directions (none came in 15 s
each [confirmed]). MMI3GApplication reads a joystick from bytes 8–9
(`processJoystick`, 0x822B70C) and an older touch panel from bytes 10–12
(`processTouchpanel`, 0x822B87C), for other cars [RE].

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
- Bytes 13–26 are not panel controls. MMI3GApplication's KeyPanel
  (`CBitInterpreter`, dispatcher 0x822C038) reads them as [RE]:

  | byte | meaning |
  |---|---|
  | 13 | "tip display status" (0x822ACA0) |
  | 14 | message kind: `01` panel and wheel, `02` touch pad (below) |
  | 15 | "turn mechanic status" (0x822AC34): the motorised screen, below |
  | 16 | source: `1`/`2` panel only, `3` steering wheel only, `0` both |
  | 23–26 | steering wheel, below |

  The decoder ignores bytes 16–26.

### Steering wheel (MFL): bytes 16 and 23–26

The multifunction steering wheel's keys arrive on ch8 as plain 27-byte
messages, read by the KeyPanel's MFL handler (0x822B1B4) [RE]:

| byte | meaning |
|---|---|
| 16 | `03`: a wheel message |
| 23, 24 | key codes of up to two keys (`00` = none, `22` = NO_KEY) |
| 25 | events: low nibble for byte 23, high nibble for byte 24 |
| 26 | high nibble variant, low nibble brand (`11` = variant 1, brand 1) |

Codes `06` and `12` are rollers (0x822B0FC): their event nibble is signed
ticks, `1` = +1, `F` = −1. Codes `01`–`08`, `10`–`22` and `F0`/`F1` are keys,
with the event nibble as press/hold/release. A message with code `00`
follows each event as the release. The MMI maps each code to its own key id
(0x822AFE0) and logs `KEY: 0x%x(MFL) -> 0x%x(DSI)` or `ROLLER: ...`.

Logged on ch8 while the volume was changed [confirmed: the messages]:

```
.. 01 02 03 01 00 00 00 00 00 12 00 01 11    roller 12, +1
.. 01 02 03 01 00 00 00 00 00 00 00 00 11    release
.. 01 02 03 01 00 00 00 00 00 12 00 0f 11    roller 12, -1
```

Roller `12` is the steering wheel's volume roller: one message per click,
`+1` (nibble `1`) rolled up, `-1` (nibble `F`) rolled down, each followed by
the code-`00` release [confirmed: hello7's guided steps, 3 clicks each
way]. In an earlier run the wheel steps got no ch8 message at all; that
run's wheel was probably not used.

### Touch pad: byte 14 = `02`

The pad recognises on its own and reports results, not raw samples. A pad
message is a 27-byte header with byte 14 = `02`, a **subfunction** in
byte 27 and its data from byte 28 on (28–36 bytes in all). The KeyPanel's
handler (0x8227B5C) knows these [RE]:

| byte 27 | data | meaning |
|---|---|---|
| `CA` | x, y | finger down at x, y (0–255) |
| `CB` | | finger up |
| `30` / `31` | key | virtual key pressed / released |
| `3D` / `3E` | key | finger moved onto / off a virtual key |
| `D3` | `00` / `01` | tap / double tap |
| `38` | 1–8 | 2D scroll: N, NE, E, SE, S, SW, W, NW |
| `48` | axis 1–4, signed step | 2D slider; axis 1 acts as the volume encoder, 2 as the main knob, 3/4 as encoders `4C`/`4D` |
| `39` | 0–8 | border status: CENTER, N .. NW |
| `C5` / `C6` | x, y | absolute / relative coordinates |
| `C7` | begin, coding (`01` = UTF-8), text | a recognised handwritten character |
| `C4` | mode | the recognizer mode now (the answer to a mode change) |
| `C2` | 5 chars | the recognizer language |
| `C1` / `C8` | setting, 32-bit value | a generic setting |

Virtual keys (0x82279A0): `37` KEY1, `3D` KEY2, `39` KEY3, `3A` KEY4, `3B`
KEY5, `3C` KEY6 (the radio presets), `50`–`58` CENTER, N .. NW.

**The HMI chooses what the pad reports.** It sets a recognizer mode per
screen by writing to ch8 itself (0x82288B8, `KeyPanel:
requestSetRecognizerMode`):

```
22 00 00 00 00 00 02 00 C4 <mode>        10 bytes, open/write/close per command
```

The pad answers with subfunction `C4 <mode>`. MMI3GApplication passes on
modes 6, 9, 10, 11, 12, 13, 16, 17 and 18 (0x8226994); every other value
becomes 0. The HMI's mode names (lsd.jxe) include `RECOGNIZERMODE_OFF`,
`_RADIO`, `_RADIO_AND_MAP`, `_MAP_PANNING`, `_NAV`, `_TEL`, `_FREETEXT`,
`_TP_ENCODER_XY`, `_TP_ENCODER_FADER` and `_TP_ENCODER_DVD`; which number is
which is open. The language is set the same way with `C2` and 5 bytes. The
panel's key lights use the same header with byte 6 = `01` (0x8228AF4).

Logged on ch8 [confirmed], read with the table above: a finger drawn
across the pad on the radio screen gives `CA 84 C6` (down at 132, 198),
then `3D`/`3E` pairs as it crosses preset zones (KEY6, KEY4, KEY2, KEY1,
...), then `CB`. A main-knob press or a soft key changes the HMI's screen,
and ~0.6 s later the pad answers `C4 00` and `C4 06` / `C4 09`: the HMI
switched the mode off and to the new screen's [inferred].

**What each mode reports** [confirmed: hello7 wrote each mode itself, then
a line, the letter A and a tap were drawn]:

| Mode | Answer | What the pad sends |
|---|---|---|
| 6 | `C4 06` | the 9 direction keys (`30`/`31`, `3D`/`3E` with `50`–`58`), relative motion `C6`, tap `D3` |
| 9 | `C4 09` | the six preset keys KEY1–KEY6 (`30`/`31`, `3D`/`3E`), nothing else |
| 10 | none | not taken: the pad stays in its mode |
| 11, 12, 13 | `C4 0B`..`0D` | absolute positions `C5` while the finger moves, a recognised character `C7` after it lifts, tap `D3` |
| 16 | none | not taken: the pad stays in its mode |
| 17, 18 | `C4 11` / `C4 12` | the direction keys and the 2D slider `48` (axes 3 and 4), tap `D3` |

- Before each new mode the pad first answers `C4 00`.
- With no mode set by an app, a tap and a double tap give the same
  messages (`CA`, `30`/`31`, `CB`; no `D3`), and a line gives `3D`/`3E`.
- The HMI's map screen sets `0`, then `18`, then the language (`C2
  "pl_PL"`) [confirmed]. So 18 is the map's mode, 9 the radio's (presets,
  earlier runs) [inferred]. A mode an app set stays until the HMI sets
  another: on the radio screen after hello7's mode 18 the pad kept sending
  sliders.
- `C7`'s data is not plain text after its first byte: for a drawn "A" the
  first text bytes were `5`, `4`, `c` and `3` in the different modes, with
  more bytes behind them that are not letters (`1e 0f 00 00 20 5f 2d ..`).
  Probably several candidates in a fixed layout, or cut short by the
  message length [inferred]; the layout is open.



### Byte 15: the motorised screen

MMI3GApplication logs byte 15 as the "turn mechanic status" (0x822AC34)
[RE]: the motor that moves the screen out of and into the dashboard. It is
`02` in normal operation. With nothing touched, ch8 sends a 29-byte touch pad message (bytes
14–15 = `02 02`), then `01 03` 0.2–0.6 s later and `01 00` about 3 s after
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

So `03` is the screen moving and `00` the screen retracted [inferred]: the
"motor running down" tone is the screen folding away at ignition off, and
the unit can run on for minutes after it. The decoder's Power event still
gives an app a few seconds' warning that the car is being switched off.
To confirm: switch the ignition off on purpose during a run and watch the
screen.

### Several readers

dev-ipc gives every reader of a channel its own copy of each message, and
a reader that keeps the channel open without reading stalls it for every
reader after 32 messages ([docs/vehicle-data.md §2.2](../../../docs/vehicle-data.md)).
For ch8 that includes MMI3GApplication, so: one reader per process
(quit.c's), always reading, and no ch8 fd inherited by a child that won't
read it: quit.c opens ch8 with `O_CLOEXEC` (so do hello7 and hello11 for
their channels), and the runner spawns its apps with only
stdin/stdout/stderr.

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
short. `quit.c` reads ch8 and offers the
bytes to an app observer (`quit_init`): the hub, or a plain callback
(hello5 counts volume detents that way). **hello7** is the input tester:
its guided mode named the table above, one control per step, and now
asks the open touch pad and steering wheel questions.

# hello5

Sound from an app. Plays a generated ping train (`pulse.c`: 0.5 s ping every
2 s) on io-media's `mixer:DSPOUT_TDM1_20_SAD`, one step at a time, each with
an on-screen question for the listener.

- **Run:** `hello5 <sd_path> [steps] [outputs]` (default steps `pmntgv`, every step but `d`, which is disabled)
- **Output:** `var/hello5.log` (`RESULT` lines), `hello5_<step>.log`, step `p`/`d` dumps
  (`hello5_ipc.txt`, `hello5_pidin.txt`, `hello5_broker.txt`, `hello5_helper.log`, …)
- **Screen:** steps `p` and `d` show what they are doing now. Step `p`:
  which tool or listing runs and how long it may take. Step `d`: what the
  helper does (`ConnectRequest to chid 14, server id 0x00020027`, …), for
  how long so far, when hello5 gives up on it, and its last note (`try 1
  of 2: …`), redrawn every second. The same lines are in the logs
  (`step p: now …`, `helper -> wait …`).

![Step g: output survey, group 2 playing](img/chime-group.jpg)

## Verifies

- `m`: does our sound mix with the radio or replace it?
- `n`: hello5's own io-media client (`iom.c`): own volume, queued files, event timing.
- `t`: does it play over Bluetooth music?
- `v`: does it follow the car's volume knob?
- `g`: which mixer outputs / io-audio devices are audible (N chimes on output N).
- `p`: which channels MMI3GApplication, MMI3GNavigation and the servicebroker
  connect to and receive on (`ipcprobe.c`).
- `d` (**disabled**, hello5 refuses it): sound over a lowered radio: ask audio
  management for the nav voice channel over HBDSI, as the navigation does, and
  play on io-audio `navi`.

## Results

**Works**
- `TDM1_20_SAD` is heard clearly, with the radio or Bluetooth music on.
- Its level follows the car's volume knob.
- hello5's own io-media client plays; queued files play.
- The screen comes back over the MMI's Media screen (`screen_set_reshow`).

**Doesn't work**
- No mixing: `TDM1_20_SAD` mutes the radio and music while it plays.
- The graph volume does nothing (io-media's mixer volume is an empty stub):
  scale the samples to be quieter.
- Queued files add ~350 ms of silence each, and io-media sends one `FINISHED` per queue.
- QSA (`libasound`): every device takes the data, nothing is heard.
- Other mixer outputs are silent or mute the car's audio; `TDM1_51_SAD` won't attach while the radio plays.
- The servicebroker doesn't know `SGAudioManagement` (status 2).
- Looking up the three processes' pids through `/proc` found none of them
  (`EBADF`); the `sloginfo` / `pidin arg` fallback finds all three, and
  step `p` then lists their channels (docs/hbdsi.md §6.1).
- Step `d`: a memory scan of MMI3GApplication (~70 s) finds no server id,
  so step `d` no longer scans; it tries the bootstrap table's two ids on
  chid 14. The ConnectRequest to chid 6 gets an all-zero reply and no
  connect-back; the one to chid 13 **never returns**, and the sender can't
  be stopped even by SIGKILL (hbdsi.md §6.1). Pings on `navi` without a
  channel are not heard.
- `pidin -p <pid> fd` never finishes for MMI3GApplication or
  MMI3GNavigation (killed after ~18 s), so step `p` no longer runs it.
- `testapp-file`'s exit code is always 1, success included.

**Not explained**
- In `t` and `v` the first ping can come ~12 dB under the rest (amplifier fade-in? [inferred]).

**Step `d` breaks MMI3GApplication, so it is disabled** (the log lines
[confirmed]; that our sends caused them [inferred]: nothing else wrote to the
process). The
ConnectRequest to chid 14 with `0x00020027` gets an all-zero reply; the one
with `0x00010027` times out. Minutes later, at the next on/off state change
(`OnOff new system state is 4`, `6`), MMI3GApplication logs `failed to
allocate 977613627 bytes` and `terminating thread "Receive"`. "Receive" is
the thread that takes HBDSI messages from the other native processes (the
standard `Receive / Send / Trace / MainThread / DSIReceive / DSISend` set,
thread-name table at `0x8A308EC`; the log line comes from `operator new`,
`0x807F11A`). Without it the nav layer stays on screen and the unit does not
power down when the ignition goes off and the door opens; a reboot fixes it.
hello5 now refuses step `d` and its helper (`@duck`). The helper code
(`duckhelper.c`, `ducklink.c`) is kept for reference: every HBDSI send ran in
a copy of hello5 without a screen, with a per-send deadline.

Details: [docs/platform-notes.md §9](../../docs/platform-notes.md) (audio),
[docs/hbdsi.md](../../docs/hbdsi.md) (step `d`), knob/button decoding moved to [hello7](../hello7/).

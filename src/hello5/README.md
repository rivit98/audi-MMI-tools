# hello5

Sound from an app. Plays a generated ping train (`pulse.c`: 0.5 s ping every
2 s) on io-media's `mixer:DSPOUT_TDM1_20_SAD`, one step at a time, each with
an on-screen question for the listener.

- **Run:** `hello5 <sd_path> [steps] [outputs]` (default steps `mntv`; the card runs `pd`)
- **Output:** `var/hello5.log` (`RESULT` lines), `hello5_<step>.log`, step `p`/`d` dumps
  (`hello5_ipc.txt`, `hello5_pidin.txt`, `hello5_broker.txt`, `hello5_services.txt`, …)

![Step g: output survey, group 2 playing](img/chime-group.jpg)

## Verifies

- `m`: does our sound mix with the radio or replace it?
- `n`: hello5's own io-media client (`iom.c`): own volume, queued files, event timing.
- `t`: does it play over Bluetooth music?
- `v`: does it follow the car's volume knob?
- `g`: which mixer outputs / io-audio devices are audible (N chimes on output N).
- `p`: which channels MMI3GApplication, MMI3GNavigation and the servicebroker
  connect to and receive on (`ipcprobe.c`).
- `d`: sound over a lowered radio: ask audio management for the nav voice
  channel over HBDSI, as the navigation does, and play on io-audio `navi`.

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
- Looking up the three processes' pids through `/proc` found none of them.
- `testapp-file`'s exit code is always 1, success included.

**Not explained**
- In `t` and `v` the first ping can come ~12 dB under the rest (amplifier fade-in? [inferred]).

**Not yet confirmed on the car**
- Steps `p` and `d` with the widened pid lookup (`sloginfo`, `pidin arg`,
  `procdiag`) and the direct ConnectRequest to MMI3GApplication.

Details: [docs/platform-notes.md §9](../../docs/platform-notes.md) (audio),
[docs/hbdsi.md](../../docs/hbdsi.md) (step `d`), knob/button decoding moved to [hello7](../hello7/).

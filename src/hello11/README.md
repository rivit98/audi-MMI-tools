# hello11

Can a native app tell, without the Java HMI:

1. that the **rear camera** is wanted,
2. that the car's **lights are on**, which is what the HMI uses for its night
   map ([docs/vehicle-data.md](../../docs/vehicle-data.md) §5.2), and
3. that it is **dark outside**: the light sensor, and the display's own
   dimming, which is a separate value (`Brightness`, source not traced)?

Night mode for Android Auto would follow 2 or 3.

hello11 is **read only**. It opens four sources and writes nothing to any
channel or device. Written in C++ ([docs/cpp-apps.md](../../docs/cpp-apps.md)).

| Source | What it gives | Expected signal |
|---|---|---|
| `/dev/ipc/ch4` | the IOC's telegrams to MMI3GApplication's MOST subsystem, from the camera library's reader (`camera::subscribe_ch4`, `ipc4.h`, vehicle-data.md §5.1): every function block's payload and every BAP function | lights: `DA01.0C0D` first byte; camera: `BAP 0B.1A` (rear-camera.md §3.3) |
| `/dev/ipc/ch5` | the vehicle-signal records by tag and subtag (`sources.h`, vehicle-data.md §3); gyro, accelerometer and clock left out | light sensor: maybe `ch5 09.xx RainSens` |
| `/dev/ndr/name/sensor/ReverseGear` | ndr's reverse flag, from ch5 (vehicle-data.md §4) | reverse gear |
| `/dev/slog` | dev-videoctrl's lines (`common/camera`); lines about brightness, dimming, day / night (`sources.h`), keyed with their numbers as `#` | the HMI shows the camera: ground truth; the display dims |

**Channel safety.** MMI3GApplication reads ch4 too, ndr reads ch5. dev-ipc gives every reader
its own copy ([vehicle-data.md §2.2](../../docs/vehicle-data.md)), so our
reads take nothing from them, as long as we keep reading (the threads do nothing else). As a guard, each
telegram has a running counter: more than one gap in five telegrams stops ch4
reading for the rest of the run (`camera: ch4 STOP` in the log).

## Steps

The first screen lists the whole test (the steps below, short) and waits for
one knob click (90 s at most). The driver then turns the main knob **one
click** at the end of each step. A step
without a click ends after its time limit (`TIMEOUT` in the log).

| # | Step | Kind | Limit |
|---|---|---|---|
| 1 | parked in P, ignition on, light switch at 0, touch nothing | quiet | 20 s |
| 2, 4 | headlights on | lights on | 60 s |
| 3, 5 | light switch to 0 | lights off | 60 s |
| 6 | light switch to AUTO (it is light: the headlights stay off) | switch AUTO | 60 s |
| 7, 9 | light sensor covered until the headlights come on, +10 s | sensor covered | 120 s |
| 8, 10 | sensor uncovered until the headlights go off | sensor free | 180 s |
| 11, 13 | R, until the camera picture is on | reverse | 150 s |
| 12, 14 | P, until the camera picture is gone | camera off | 150 s |
| 15 | touch nothing | quiet | 20 s |

Every step's instruction is on the screen while it runs, with the live
values (light flag, RainSens, camera, the last brightness slog line).

Our screen shows the step and the live values. It is hidden while the HMI
shows the camera (never hide the camera) and comes back after it.

**The answer** is the summary (last screen and `hello11.log`): per kind, the
keys that changed in **every** step of that kind and in **no** quiet step,
with their first change. "lights on: DA01.0C0D/C" with `[00 ..] -> [01 ..]`
would confirm §5.2; "reverse: BAP 0B.1A" that the camera's session can be
read natively. A key under "sensor covered" that is not under "lights on" follows
the sensor, not the headlights: the darkness signal.

## Output

| File | What |
|---|---|
| `var/hello11.log` | steps, changes of the watched keys (DA01 lights, BAP LSG 0x0B, ndr), per-step key lists, summary |
| `var/hello11/changes.txt` | every change of every key |
| `var/hello11/ch4.txt` | every telegram, decoded |
| `var/hello11/ch4.bin` | the raw bytes as read: `u32 ms, u32 length, bytes` per read |
| `var/hello11/ch5.bin` | the same for ch5 (`tools/ch5dump.py` decodes the records after stripping the headers) |
| `var/hello11/slog.txt` | the brightness / day-night slog lines, including those from before the start |

## State

Run on the car once [confirmed]:

- **Camera: yes.** BAP `0B.1A` and `0B.16` follow every camera session,
  about 1 s ahead of the slog ([rear-camera.md §3.3](../../docs/rear-camera.md)).
  The camera library now uses it.
- **Lights and light sensor: no.** Nothing on ch4 or ch5 changed only with
  the light switch or the covered sensor; `DA01.0C0D` steps like a counter,
  also in quiet steps. `/dev/ndr/.../ReverseGear` is not readable (ENOSYS).
- Side finds: BAP `0A.12` / `0A.13` / `0A.15` change with reverse (the
  parking aid's LSG), BAP `11.11` is the clock (`0d 0e 1d` = 13:14:29).
- The camera came up during the quiet and lights steps as well, so the
  per-kind summary lists came out empty; the step-by-step changes in
  `hello11.log` carry the answer.

Since then the ch4 reading moved into the camera library
(`camera::subscribe_ch4`); built and host-tested (`tests/test_hello11.cpp`,
`tests/test_camera.cpp`).

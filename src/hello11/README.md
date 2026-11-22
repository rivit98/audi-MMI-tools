# hello11

Can a native app tell, without the Java HMI:

1. that the **rear camera** is wanted, and
2. that the car's **lights are on**, which is what the HMI uses for its night
   map ([docs/vehicle-data.md](../../docs/vehicle-data.md) §5.2), and what
   Android Auto's night mode would follow?

hello11 is **read only**. It opens three sources and writes nothing to any
channel or device. Written in C++ ([docs/cpp-apps.md](../../docs/cpp-apps.md)).

| Source | What it gives | Expected signal |
|---|---|---|
| `/dev/ipc/ch4` | the IOC's telegrams to MMI3GApplication's MOST subsystem: every function block's payload and every BAP function (`ipc4.h`, vehicle-data.md §5.1) | lights: `DA01.0C0D` first byte; camera: `BAP 0B.17` (`ff 01` request, `xx 00` end, rear-camera.md §3.1) |
| `/dev/ndr/name/sensor/ReverseGear` | ndr's reverse flag, from ch5 (vehicle-data.md §4) | reverse gear |
| `/dev/slog` | dev-videoctrl's lines (`common/camera`) | the HMI shows the camera: ground truth |

**ch4 safety.** MMI3GApplication reads ch4 too. If dev-ipc shares the
telegrams between readers instead of giving each reader a copy, we would take
some away from it. Each telegram has a running counter, so hello11 sees that
at once: more than one gap in five telegrams stops ch4 reading for the rest of
the run (`ch4: STOP` in the log). The system dump's 3 s `cat` of ch4 saw no
gaps, so a copy per reader is likely [inferred].

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
| 6, 8 | R, until the camera picture is on | reverse | 150 s |
| 7, 9 | P, until the camera picture is gone | camera off | 150 s |
| 10 | touch nothing | quiet | 20 s |

Our screen shows the step and the live values. It is hidden while the HMI
shows the camera (never hide the camera) and comes back after it.

**The answer** is the summary (last screen and `hello11.log`): per kind, the
keys that changed in **every** step of that kind and in **no** quiet step,
with their first change. "lights on: DA01.0C0D/C" with `[00 ..] -> [01 ..]`
would confirm §5.2; "reverse: BAP 0B.17" that the camera request can be read
natively.

## Output

| File | What |
|---|---|
| `var/hello11.log` | steps, changes of the watched keys (DA01 lights, BAP LSG 0x0B, ndr), per-step key lists, summary |
| `var/hello11/changes.txt` | every change of every key |
| `var/hello11/ch4.txt` | every telegram, decoded |
| `var/hello11/ch4.bin` | the raw bytes as read: `u32 ms, u32 length, bytes` per read |

## State

Built and host-tested (`tests/test_hello11.cpp`; the parser also decodes the
system dump's ch4 capture with no gaps and no skipped bytes). Not yet run on
the car.

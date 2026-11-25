# camera

A C++ library: is the rear camera up right now, for any app that shows
something or writes VP1. It reads two sources and never writes anything.
It also owns the process's one `/dev/ipc/ch4` reader. Not an app: link the
Makefile's `CAMERA` group (`src/common/camera` is on every app's include
path). The why: [docs/rear-camera.md](../../../docs/rear-camera.md) §3.3 and
§9.1.

| Source | Says | When |
|---|---|---|
| `/dev/slog`, dev-videoctrl's lines | the HMI owns VCAP 0 (`VP0: source changed to N` … `VP1: vcap disabled`) | the authority |
| `/dev/ipc/ch4`, BAP `0B.1A` | the camera's control unit runs a session (`x1 ..` on, `x0 ..` off) | 0.9–1.3 s before the slog's start, 0.6–1.1 s before its end |

The camera is **up** while either says so. BAP is the early warning: an app
gets off VP1 and the screen before the HMI switches the capture, not after.

- `vcap.h`: `VcapTracker`, pure. Slog events in, VCAP 0 owned or not.
- `ipc4.h` / `.cpp`: ch4 framing, the MOST header, BAP streams, pure.
- `bap.h`: `rvc_state()`, pure. A ch4 telegram or BAP function in, the
  camera's session state out.
- `state.h`: `Combiner`, pure. The two signals in, `active()` / `clear()`
  out, with the guards below.
- `takeover.h`: `Takeover`, pure. What an app with a picture does: retake
  the screen after the camera, show again, give back on a race.
- `carsig.h`: `carsig::Logger`, pure. ch4's car functions (not BAP) in,
  throttled change lines and the night state (DA01.0C0D byte 0) out. Used
  by audi_stream_receiver's `carlog.cpp`.
- `camera.h` / `.cpp`: the reader threads, the state, observers.
- Tests: [tests/test_carsig.cpp](../../../tests/test_carsig.cpp) and [tests/test_camera.cpp](../../../tests/test_camera.cpp), on ch4
  telegrams and slog lines logged on the car.

## Using it

```cpp
camera::start();                         // reads the slog so far: active() is right at once
if (camera::active()) { /* write nothing to VP1, show nothing */ }
if (camera::clear())  { /* not up, and the HMI had 1 s to show its own screen */ }

camera::Takeover t;                       // an app with a picture, every tick:
switch (t.tick(have_picture, camera::active(), camera::clear(), log_ms())) { ... }

int id = camera::subscribe([](const camera::Event &e) { ... }, "myapp");   // changes
int id4 = camera::subscribe_ch4(on_telegrams, "myapp");                     // ch4's telegrams
camera::stop();
```

- `start(false)`: the slog only, no ch4.
- `status()`: `vcap`, `bap`, `active`, and `bap_unconfirmed` (BAP sessions the
  HMI never followed).
- `Event`: `up`, `why` (`"BAP"`, `"VCAP 0"`, `"BAP alone"`, `"BAP gone"`),
  `ms`. Callbacks run on a reader thread with the subscriber lock held: short,
  no blocking, no `subscribe` / `unsubscribe` from inside one.
  `unsubscribe` returns once no callback of that id is running.
- `subscribe_ch4`: every ch4 read (raw bytes, `log_ms()` and the telegrams it
  completed), for code that studies ch4 (hello11). Never open ch4 a second
  time in a process: the reader below already keeps up.
- `ch4_stats()`: the reader's state (`reading`, `no open`, `read failed`,
  `stopped: gaps`, `stopped`) and the telegram counters.

Not started, or no `/dev/slog`: `active()` is false and `clear()` true, as if
there were no camera. No ch4: the slog alone.

The log (`log.h`) gets each source's change and each change of the result
with its subscribers: `camera: BAP 0B.1A session on`, `camera: up (BAP) ->
runner`, `camera: VCAP 0 taken by the HMI (slog)`, `camera: off (VCAP 0),
clear in 1000 ms -> nobody`.

## Guards

- **Lost "off".** BAP is sent only on a change and ch4 can lose telegrams.
  When the HMI lets VCAP 0 go after taking it, the session is over whatever
  BAP said last.
- **"On" the HMI never follows.** A BAP session without VCAP 0 for 5 s
  (`kBapAloneMs`) is dropped (`BAP alone`, counted). The slog still catches a
  camera that starts later.
- **Lost "on".** Only the early warning is lost: the slog sees the camera.
- **ch4 safety.** dev-ipc gives every reader its own copy, but a reader that
  stops reading with the channel open stalls it for every reader,
  MMI3GApplication included ([vehicle-data.md §2.2](../../../docs/vehicle-data.md)).
  The reader thread only reads, parses and hands on; it logs only on a
  change. It is opened close-on-exec, so children don't inherit it. More than
  one gap in five telegrams (after 20) means dev-ipc is sharing, not
  copying: the reader stops (`camera: ch4 STOP`), the slog goes on alone.
- **Stopping.** `stop()` ends the slog thread at once. The ch4 thread is
  blocked in `read()` until the next telegram; it closes the channel then
  (detached; process exit ends it as well). A `start()` before that reuses it.

## Users

runner (menu hides), audi_stream_receiver and android_auto (`Takeover`, hw
video), `vout` / `hw_stream` (no VP1 writes while up), hello7 (`cam.cpp`, a C
wrapper), hello11 (status and the ch4 telegrams). hello9 and hello10 link it
through `LIVE` but don't start it: their tests run parked.

## State

The slog part is confirmed on the car (rear-camera.md §9.1). The BAP signal
is confirmed in hello11's ch4 log (three sessions). The combined library is
built and host-tested, not yet run on the car.

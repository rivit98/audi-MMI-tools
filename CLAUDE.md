# CLAUDE.md

Native C / C++20 apps for one Audi MMI 3G+ head unit (QNX 6.3.2, SH7785
SH-4A, single core, little endian), plus an Android sender app. There is no
QNX SDK: Debian's SH4 GCC 16 links against stubs made from the unit's own
libraries. Start with [README.md](README.md), which is the app index, and
[docs/native-apps.md](docs/native-apps.md), the developer guide with every
platform rule. The target unit: MMI 3G+ 9411, firmware HN+R_EU_AU_P1001,
board E1 (the firmware tree's `51` directory, the Makefile's `FW`).

## Commands (everything runs in Docker, never with host tools)

```sh
make sdk                         # once: mmi-sdk image (toolchain + sysroot from the firmware)
make                             # cross-compile all apps into build/
make test                        # host unit tests (ASan/UBSan), incl. protobuf + live-stream checks
make sd SD_APPS="audi_stream_receiver"   # build/sd/ for the car; default list in the Makefile
make addr APP=x ADDR="0x804..."  # crash address -> function, file:line (apps are ET_EXEC, unstripped)
make apk / apk-test / apk-install        # Android sender (android/Makefile, adb in a container)
make aoa-host                    # desk: the PC plays the MMI for the phone app (stop host adb first)
make aa-host                     # desk: the PC plays the Android Auto head unit
make plot RUN_DIR="runs/<run>"   # audi_stream_receiver logs -> PNG plots + summary (into the run dir)
make shell                       # shell in the SDK container
```

A new toolchain gets its own image and a Makefile next to its sources, and the
parent Makefile only delegates (`$(MAKE) -C android`). Never fall back to host
gradle, adb or gcc.

## Layout

- `src/<app>/`: one app's own logic + `README.md` (what it does, how to run it, state on the car).
- `src/common/`: shared code linked **by group** (`BASE`, `SCREEN`, `LIVE`, ...; table in
  native-apps.md §4 and the Makefile comment). Each app's rule names its groups.
- Each app builds with **one compiler command** over all its sources (C files compiled as C,
  the rest as C++), so `build/` has no `.o` files. That is expected.
- `tests/`: host tests of the pure parts (protocol, parsing, stream planning, camera tracker).
  Put logic that doesn't touch car APIs in pure files/headers and test it here first.
- `sd/`: `run.sh` (ksh, the on-car runner), `screens.sh`, `apps.conf` (timeout|args|title|text).
- `android/`: the "Audi streaming" phone app (Kotlin, package `mmi3g.audistreaming`).
- `tools/`: desk tools (`aoa_host.py`, `ch5dump.py`, Ghidra helpers).
- Inputs that are not in git: the extracted firmware and openqnx source (the Makefile's `FW`,
  `FWEFS`, `OQ`), and mbedTLS + aasdk's certificate, which the Makefile fetches with pinned
  SHA-256s. Car logs and the user's working notes are kept outside git too. Never commit
  them, and never refer to them from committed files.

## Platform rules that bite (details in native-apps.md §6–7, cpp-apps.md)

- **Divide by zero is a fatal SIGFPE**, float too (`x/0.0f` is not inf). Guard every denominator.
- No `fork()` in threaded code (`spawnvp` via `proc.c`), no signal handlers (`sigwait` thread,
  `quit.c`), no TLS (`thread_local`: the build fails), no `<math.h>` in the sysroot
  (`src/common/qnx/math.h`).
- C++: static libstdc++ from GCC 16, LL/SC atomics are the default. Never add `-m4` or another
  `-matomic-model`: gUSA loses updates. QNX `std::mutex` needs the SDK's init/destroy fix
  (sync objects reused at the same address).
- `sdk/qnx-elf-fix.py` runs after every link and **deletes** a binary that has TLS or IFUNC.
  `build/.canary` stops the build if the GCC 12 `a==0&&b==0` miscompile comes back.
- 64-bit types are 4-byte aligned (QNX 6.3.2 ABI).
- SD writes are slow (~12 ms per fsync'ed log line). Keep `log_msg` out of per-frame paths:
  use a RAM ring + low-priority writer thread (`audi_stream_receiver/perf.cpp`).
- Use `CLOCK_MONOTONIC` (`log_ms()`). The wall clock is 1970 after every cold boot, so file
  dates and slog timestamps in car logs mean nothing.
- **Give the screen back on every exit path.** Displayables outlive the process.
- **Never hide the rear camera.** While the HMI owns VCAP 0 (`camera::active()`, from
  dev-videoctrl's `/dev/slog` lines), write nothing to VP1 and show nothing. Retake the screen
  1 s after the camera goes off (docs/rear-camera.md §9.1).
- Hardware video: DM642 H.264 Baseline up to 720 wide (800 is refused), only through a CS-MP4
  stream served by our own resource manager. io-media queues ~9 frames, so keep the frame rate
  steady (docs/phone-mirroring.md §7.10–7.13).
- Nothing touches flash, `srv-starter` or system binaries. Everything runs from the card.

## Conventions

- **Never use the user's nickname** in identifiers, strings, paths, package ids, log names or
  docs. The repo's own directory name is the only exception. Neutral identities: run dir
  `/tmp/mmi_apps`, AOA manufacturer `mmi3g`, packages `mmi3g.*`, run log `var/run.log`.
- Types: `src/common/types.h` (`u8`..`u64`, `i8`..`i64`, `usize`, `isize`, `f32`, `f64`), never
  `uint32_t`/`size_t`/`ssize_t`/`double` etc. in our code (third-party headers excepted).
- Header guards = directory + file (`COMMON_LOG_H`, `HELLO5_PULSE_H`), never a bare `X_H`.
- New app: `src/<app>/` + README, Makefile rule (+ `CXX_APPS` for C++), `sd/apps.conf` line,
  host tests (native-apps.md §4 "Adding an app").
- **Commit every finished piece of work automatically**, without asking, so progress is easy
  to track: one commit per logical step (a fix, a feature, a docs update after a car run).
  Run `make test` (and `make apk-test` for android/) first when code changed. Stage only
  the files that step touched (`git add <paths>`, never `git add -A`), so unrelated changes
  in the tree stay out. Message: a short imperative subject (`android_auto: seed the DRBG from
  the jitter source`) and a body with the why. Never push and never rewrite history unless
  asked.

## Docs style

- Docs state findings as **timeless facts**: "A doesn't work (why); B works". No dates, no run
  history, no references to car-log folders or anything else outside git. "Built, not yet confirmed on the
  car" is fine as a status.
- Label claims **[confirmed]** (measured on the car), **[inferred]** or **[unverified]**, and
  name the measurement or binary address behind each one. android-auto.md uses its own labels
  (`[repo]`, `[recalled]`, `[source]`, `[estimate]`).
- No duplication between .md files. README.md is a short index pointing to `src/<app>/README.md`
  and `docs/`. Each fact lives in one place.
- Plain, short sentences. Write units and numbers out (`~300 ms`, `720×432`).

## Analysing a car run

The user copies the card's `var/` folder and points at it. Typical files:

| File | What it is |
|---|---|
| `run.log` | run.sh: apps, timeouts, start errors (`mkdir /tmp` errors are harmless) |
| `<app>.log` | the app's log; `[N ms]` = monotonic ms since the app started |
| `<app>_out.log` | stdout/stderr (usually identical to `<app>.log`) |
| `usb_slog_N.txt` | slog snapshot taken at each USB removal (not at the end of the run) |
| `<app>_slog.txt`, `summary.txt` | only if the app ended normally under run.sh |
| `*.h264`, `*_perf.csv` | saved video (check with ffprobe/ffmpeg), perf records |

How to read a run:

- A log that stops with no quit/stop line means the card was pulled or the power went off.
  That isn't a crash. Crashes leave `*.core*` in `var/` and a `run fault` line in slog.
- First check the phone APK version: the `rx HELLO ... build <version>` line. An old APK on the
  phone has invalidated phone-side results before.
- audi_stream_receiver H.264: read the `h264: config:` lines (capture mode, frame cap), the
  `h264: 10.0 s:` windows (fps, skips, `lag min/median/max`), `stream ended` totals (refused,
  bad, ahead of the clock), and `camera:` / `videoctrl` lines. `lag` is MMI-side only: the
  phone's capture, encoder, queue and USB are not in it.
- android_auto: the `aa:` phase lines, `stats:` (fps, kbit/s, rtt), `cpu:`.
- Combine the logs with the user's own observations (what they saw on screen). Then fold the
  results into the docs as facts, update the app READMEs' state and the README table, update
  the user's test list, and commit.

## Working with the user

- Every car test costs the user a trip to the car, so batch the tests. Give each one a precise
  test list (what to do, which log line answers it), and build + host-test everything first.
- The phone is a Redmi Note 8 Pro (MIUI 12.5, Android 11). Phone-side behaviour often differs
  from AOSP (VirtualDisplay resize, projection stops on lock). `adb logcat` is the best source
  for phone-side problems.
- Reverse engineering: the unit's binaries are in the firmware tree the Makefile's `FW` points
  to (rev `51`). IDA / Ghidra MCP tools are available; native-apps.md §16 has the workflow.

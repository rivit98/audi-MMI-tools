# runner

A test menu on the MMI's screen: turn the main knob to choose an app, press
it to run it. Each app is listed by its file name, and its verdict shows in
the menu afterwards. run.sh starts the runner on SD insert; it is the only
way the card runs apps. Input comes from the [buttons](../common/buttons/README.md)
library.

- **Installing an app:** copy its binary into `apps/` on the card. That's
  all: the runner lists every ELF file there.
- **Card:** `make sd` builds every app and puts all of them in `apps/`
  (their clips, tools and files where they always go), and the runner in
  `bin/`. Copy with `tools/sync_sd.sh <card>`.
- **Run:** `runner <sd_path>` (run.sh copies `bin/` to `/tmp/mmi_apps/`
  and starts it from there; no timeout)
- **Output:** `var/runner.log` (the menu, every key, every start and
  result), and per app: `var/<app>_out.log` with its
  `=== exit code / run time / verdict` lines, a line in `var/summary.txt`,
  `var/<app>_slog.txt`, core files moved to `var/`.

## The menu

- Lists every ELF file in the card's `apps/` (read once, at start). Files
  that are not ELF (READMEs, macOS `._*` files) and names outside
  `[A-Za-z0-9_.-]` are skipped, with a line in `runner.log`.
- Each line is the app's file name (Terminus 12×24), its last verdict on
  the right.
- An `apps.conf` line (`name|timeout s|arguments|text`) is optional. It
  gives the app its text, arguments and timeout, and puts it in
  `apps.conf`'s order. Apps without one follow by name: no arguments, a 1 h
  timeout (apps end themselves; quit.c's watchdog).
- Eight lines at a time; the list scrolls with the selection.
- Under the list: the selected app's last run (verdict, exit, seconds) and
  its `apps.conf` text (what to do).
- Main knob clockwise moves down, anticlockwise up, by the detents turned;
  it stops at either end. A main knob press runs the selected app.
- The last line, **Leave the menu**, gives the screen back to the MMI and
  ends the runner (and the run).
- The menu re-shows itself every second (the HMI takes the screen on some
  keys) and stays away while the HMI shows the rear camera.
- The HMI still reacts to the knob while the menu is up (README in
  [buttons](../common/buttons/README.md#the-hmi-keeps-the-input)): turning it also
  moves whatever the MMI shows underneath, and a press selects there too.
  Leave the MMI on a screen where that does no harm (for example the main
  menu of the radio).

## Running an app

The runner closes its screen, copies the app from `apps/` to
`/tmp/mmi_apps/` with mode 755, next to run.sh's copy of `bin/` (FAT has no
exec bit, the card can be pulled while it runs, and the apps look for the
unit's tools next to themselves), starts it with `<sd_path>` and the
`apps.conf` arguments, stdout + stderr into `var/<app>_out.log`, and waits.
At the `apps.conf` timeout it sends TERM, and 15 s later KILL to the app's
whole process group. An app still there 5 s after the KILL (REPLY-blocked
on a channel that ignores unblock requests, docs/hbdsi.md §6.1) is left
behind and the menu comes back. Presses made while the app runs are dropped. Then the
verdict (`OK`, `WARN`: FAIL lines in the output, `FAIL`, `CRASH`,
`TIMEOUT`, `MISSING`) goes into the out file and `var/summary.txt`, the
slog and core files are collected (`ksh -c`), and the menu comes back with
the verdict.

Pulling the SD card (or SIGTERM) stops the running app (TERM, KILL after
7 s) and the runner.

### Why `spawn()`

| Way | Verdict |
|---|---|
| `fork()` + `execve()` | **Not possible.** QNX 6.3 refuses `fork()` in a process with more than one thread, and every app has quit.c's threads (key, signals) and the camera thread. |
| `system()` | Blocks the caller and returns no pid: no timeout, no stop when the card is pulled, and a shell in between. The child inherits every fd. |
| `ksh -c` (`proc.c`'s `proc_shell`) | One more process. The pid is the shell's, so TERM may not reach the app. Quoting of `apps.conf` arguments. Still inherits every fd. |
| **QNX `spawn()`** | **Used.** One call, no fork, returns the app's pid. `fd_map` gives the child exactly stdin (`/dev/null`), stdout and stderr (the out file): QNX passes no other fd when `fd_count > 0`, so the runner's ch8, log, slog and layer manager fds stay out of the app. `SPAWN_SETSIGMASK` with an empty mask (the runner blocks SIGTERM for quit.c's `sigwait`, and a spawned child inherits the mask), `SPAWN_SETGROUP` a new process group (the KILL takes the app's own tools too). |

`ksh -c` is still used for the slog and core collection after each app:
that is shell work (globs, redirections).

## State

**[confirmed]** on the car: the menu lists `apps/`, the knob moves it, a
press runs the app, and the menu comes back after each app with its
verdict. Not yet exercised there: the menu staying away for the rear
camera, **Leave the menu**, and pressing MENU while the menu is up. The file-name
lines in the 12×24 font are built and host-tested only.

Host tests (`tests/test_runner.cpp`): `apps.conf` parsing, finding apps
(ELF check, names, order), the verdicts, the menu's movement and
scrolling.

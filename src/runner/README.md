# runner

A test menu on the MMI's screen: turn the main knob to choose an app, press
it to run it. Each app runs as it would under run.sh, and its verdict shows
in the menu afterwards. Input comes from the [buttons](../common/buttons/README.md)
library.

- **Installing an app:** copy its binary into `apps/` on the card. That's
  all: the runner lists every ELF file there.
- **Card:** `make sd` builds every app and puts all of them in `apps/`
  (their clips, tools and files where they always go); run.sh starts only
  the runner. Copy with rsync (README.md, "Build and run").
- **Run:** `runner <sd_path>` (run.sh does it, timeout 24 h)
- **Output:** `var/runner.log` (the menu, every key, every start and
  result), and per app what run.sh writes: `var/<app>_out.log` with its
  `=== exit code / run time / verdict` lines, a line in `var/summary.txt`,
  `var/<app>_slog.txt`, core files moved to `var/`.

## The menu

- Lists every ELF file in the card's `apps/` (read once, at start). Files
  that are not ELF (READMEs, macOS `._*` files) and names outside
  `[A-Za-z0-9_.-]` are skipped, with a line in `runner.log`.
- An `apps.conf` line is optional. It gives the app a title, its text,
  arguments and timeout, and puts it in `apps.conf`'s order. Apps without
  one follow by name: the file name as title, no arguments, a 1 h timeout
  (apps end themselves; quit.c's watchdog).
- Seven lines at a time; the list scrolls with the selection.
- Under the list: the selected app's name, its last verdict and its
  `apps.conf` text (what to do).
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
whole process group. Presses made while the app runs are dropped. Then the
same verdict as run.sh (`OK`, `WARN`: FAIL lines in the output, `FAIL`,
`CRASH`, `TIMEOUT`, `MISSING`), the slog and core files are collected
(`ksh -c`, as run.sh does), and the menu comes back with the verdict.

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
that is shell work (globs, redirections), copied from run.sh.

## State

Built and host-tested (`tests/test_runner.cpp`: `apps.conf` parsing as
ksh's `IFS='|' read` does it, finding apps (ELF check, names, order),
run.sh's verdicts, the menu's movement and scrolling). Not yet run on the car.

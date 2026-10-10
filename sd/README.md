# rivit_app: native app tests for the Audi MMI 3G+

This SD card runs a set of small test programs on the MMI 3G+ head unit,
one after another. They check that programs built for the MMI can run on
it, draw on its screen and play sound. The MMI shows what is going on
during the whole run. Every result is saved to the card, in `var/`.

Nothing is installed on the MMI and nothing is changed on it. The
programs run from the card, and the MMI should be back to normal once the
run has finished.

## Install on an SD card

1. Use an SD card formatted as **FAT32**. It is best to use an empty card:
   other MMI tools on the same card (another `copie_scr.sh`) will clash
   with this one.
2. Copy **everything in this folder** (`copie_scr.sh`, `run.sh`,
   `apps.conf`, `beep.wav`, `bin/`, `screens/`, `var/`, this file) to the
   **root** of the card. Copy what is inside the folder, not the folder
   itself.
3. Eject the card safely.

## Run it in the car

1. Switch the ignition on and wait until the MMI has fully started (you
   see the normal main menu).
2. Put the card into **SD slot 1**.
3. After a few seconds, the "rivit_app" start screen appears. Each test
   then has a screen before it, which says what it does and what you
   should see, and a screen after it with its result.
4. Wait for the **"all done"** screen to go away (a few minutes in all),
   then take the card out.

A hardware button press ends the running test early. The MMI may react to
that press as well. **If the screen freezes**, restart the MMI: hold
**MENU + the rotary knob + the top-right soft key** until it reboots.

## The tests

| Test | What it checks | What you see / do |
|---|---|---|
| `hello` | A program built for the MMI runs at all | Nothing on screen. Writes `hello.txt` with "hello world" to the card. |
| `hello2` | Drawing with the graphics chip (OpenGL ES), 20 s | A background slowly changing colour, a rotating coloured square, a white block bouncing along the top and a yellow progress bar. Saves screenshots to the card. |
| `hello3` | Graphics speed (fill rate), 3 × 10 s | Random coloured blocks that get smaller, flowing rainbow plasma, then more and more plasma layers (yellow number). Green frame counter top left. |
| `hello4` | 3D drawing, 4 × 10 s | Squares top right (green = check passed, red = failed), then two cubes cutting through each other, a gold ring with a moving light, a chequered world with crates and a grid of rings. |
| `hello5` | Sound, 30 s | The picture doesn't change. Turn the main rotary knob: every click should beep. |

The result screen after each test shows one of these verdicts:

| Verdict | Meaning |
|---|---|
| **OK** | It worked. |
| **WARN** | It ended normally but reported a failure in its log. |
| **FAIL** | It ended with an error. |
| **CRASH** | It crashed. |
| **TIMEOUT** | It ran for too long and was stopped. |
| **MISSING** | The program isn't on the card. |

## What the card brings back

| File | Contents |
|---|---|
| `var/summary.txt` | One line per test: verdict, exit code, run time |
| `var/<test>_out.log` | Everything the test printed, with its exit code and verdict at the end |
| `var/<test>_slog.txt` | The MMI's system log after the test (has the details if it crashed) |
| `var/rivit_app.log` | The log of the run itself |
| `hello.txt`, `hello2.log` … `hello5.log`, `*.png`, `*.bmp` | The tests' own logs and screenshots |

If there is nothing new in `var/` after a run, the card was not started
by the MMI. Check that the files are in the card's root folder and that
the card is FAT32.

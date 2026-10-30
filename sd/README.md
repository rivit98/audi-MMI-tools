# Native app tests for the Audi MMI 3G+

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
   `apps.conf`, `screens.sh`, `beep.wav`, `bin/`, `var/`, this file) to the
   **root** of the card. Copy what is inside the folder, not the folder
   itself.
3. Eject the card safely.

## Run it in the car

1. Switch the ignition on and wait until the MMI has fully started (you
   see the normal main menu).
2. Put the card into **SD slot 1**.
3. After a few seconds, the start screen ("MMI 3G+ native app tests")
   appears. Each test
   then has a screen before it, which says what it does and what you
   should see, and a screen after it with its result.
4. The last test is `usb_tx_test` (the phone's screen over USB). It runs
   until you take the card out, so that is how the run ends - there is no
   "all done" screen. The tests before it (hello8, hello5, then hello6) take
   about 11 minutes. If your card's run
   does not end with `usb_tx_test`, wait for the **"all done"** screen to
   go away instead, then take the card out.

Buttons and knobs do not stop a test - each one runs for its own fixed time.
To stop early, pull the card. **If the screen freezes**, restart the MMI: hold
**MENU + the rotary knob + the top-right soft key** until it reboots.

**`hello4` could freeze the whole MMI** (picture, knobs, volume) with many
rings. The ring counts that froze it are left out, and it ran clean on
2026-10-21. It is no longer run by default. If you run it and the picture
stops moving for more than 30 s, note what was on screen and the yellow
number top left, then reboot with the key combo above. Everything up to the
freeze is already on the card.

## The tests

`hello`, `lmprobe`, `hello2`, `hello3` and `hello4` have passed on the car
and are no longer run, but they are still on the card.

| Test | What it checks | What you see / do |
|---|---|---|
| `hello` | A program built for the MMI runs at all | Nothing on screen. Writes `hello.txt` with "hello world" to the card. |
| `lmprobe` | The screen hand-over, without OpenGL, 5 s | Eight vertical colour bars (white, yellow, cyan, green, magenta, red, blue, black) replace the picture. |
| `hello2` | Drawing with the graphics chip (OpenGL ES), 20 s | A background slowly changing colour, a rotating coloured square, a white block bouncing along the top and a yellow progress bar. Saves screenshots to the card. |
| `hello3` | Graphics speed (fill rate), 3 × 10 s | Random coloured blocks that get smaller, flowing rainbow plasma, then more and more plasma layers (yellow number). Green frame counter top left. |
| `hello4` | 3D drawing, about 1.5 minutes | A row of small squares top right (green = pass, red = fail). Then 10 s each: two colourful cubes, a gold ring with moving lights, a checkerboard world with crates and fog. Then a grid of coloured rings; the yellow number is how many: 1, 2, 4, 8, 12, 10 s each. **May freeze the MMI** (see above). |
| `hello5` | How our app's sound behaves next to the radio, the volume knob and music. About 2.5 minutes. | Radio on at a normal volume, and **film the screen with sound on**. Four steps; each one says on screen what to do and counts down before it starts. Each plays a short ping every 2 s with a white **PLAYING** box. **1:** listen whether the radio keeps playing *between* the pings. **2:** 12 pings played by hello5 itself: pings 4–6 should be missing, and the last three should keep the same 2 s beat. **3:** press **MEDIA** and start music (USB, Bluetooth, Jukebox, CD) during the countdown; does the music keep playing between the pings? **4:** while the pings play, turn the **volume** knob slowly, **one click at a time**: 3 clicks down, wait a few pings, then 3 clicks up; do the pings get quieter and louder too? Turn the volume back up afterwards. Note the answers (or let the video tell). |
| `hello8` | Programs written in C++ work on the MMI. About 35 s. | Nothing to do. A list of 18 checks fills in one by one: **PASS** in green, or **FAIL** in red with what went wrong underneath. The results stay up for 20 s. Take a photo if anything is red. If nothing changes for more than a minute, note the check marked **RUN**. |
| `hello7` | A tool, not run by default: names every knob and button you use. 60 s. | Turn any knob and press any button, one at a time. Each is named on screen and counted; a control hello7 doesn't know yet shows as "unknown" with its byte and bits, so note which one you pressed. The MMI reacts to them as well: MENU or BACK brings the MMI's own screens up over hello7's for the rest of its run. |
| `usb_tx_test` | The phone's screen over USB, about 2 pictures a second. Runs until you pull the card, so it is always the last test: pulling the card ends it and the run. | Install the usb_tx_test app on the phone, then plug the phone into the car's USB port. The bar at the top turns green ("Connected") once the phone accepts the "open usb_tx_test" prompt. Tap **Stream screen** on the phone and allow the screen capture: the phone's screen appears on the MMI. Turn the phone, switch apps, unplug and replug as often as you like. **Pull the card to end it.** |
| `hello6` | Whether the MMI's video chip plays phone-style video. About 8 minutes. **Park the car.** | Text screens explain every step. 10 test videos, each with a screen before it and each shown two ways, about 10 s each: **turn the main knob one click whenever you see a moving test picture** (a coloured pattern with its name and a counting number). The picture may stay black for some of them; that is a result too. A results screen stays up at the end. |

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
| `var/run.log` | The log of the run itself |
| `hello.txt`, `lmprobe.log`, `hello2.log` … `hello6.log`, `*.png`, `*.bmp` | The tests' own logs and screenshots |
| `hello6/` | hello6's details: system info, a log of each video's playback, CPU load and screen layers during each video |

If there is nothing new in `var/` after a run, the card was not started
by the MMI. Check that the files are in the card's root folder and that
the card is FAT32.

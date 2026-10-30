# what to verify next

- c++ support
- write summary how to create an app (native) and document everything + PR to the upstream
- how to make volume knob and other buttons do not trigger actions on background apps (navigation, radio, interface)
- create a keys sequence to hide/bring back my app to fg/bg
- hello8 `atomics stress` on the car (new; hello8 is back in the default run): lost updates of plain (must be > 0) / gUSA / LL/SC (must be 0). Also confirms an `sh4a`-flagged binary loads. If LL/SC holds, build all C++ (and C) apps with `-m4a -matomic-model=hard-llcs -Wa,--isa=sh4a` and relax docs/cpp-apps.md's atomics rules
- usb_tx_test screen streaming on the car: rotation bug fixed in the phone app, rerun and read the `video: #n` decode/present times
- hello5 steps t and v (28.10 ended in t's countdown): does TDM1_20_SAD attach while the MME plays music, and do the pings follow the volume knob (one click at a time)
- hello5: our graph volume (devctl 0x80080F22) has no effect on TDM1_20_SAD (28.10: volume 0 = full level). Find what `volume_ctrl` in the mixer URL does in io-media and try it
- hello5: from ~10 s into step n the radio's bass faded back in under our pings (28.10). Find out why: a longer train in step m?
- ch8 byte 15 = power state? (02 -> 03 -> 00 as the car went off on 28.10): switch the ignition off on purpose during hello7 and compare
- hello6 didn't run on 28.10 (car went off during hello5)
- name the new ch8 button bits (src/hello7/README.md, 25.10): one press each, written down in order

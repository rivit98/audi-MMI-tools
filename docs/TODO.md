# what to verify next

- write summary how to create an app (native) and document everything + PR to the upstream
- how to make volume knob and other buttons do not trigger actions on background apps (navigation, radio, interface)
- create a keys sequence to hide/bring back my app to fg/bg
- optional: rebuild libstdc++.a / libgcc_eh.a from GCC 16 source with `-m4a -matomic-model=hard-llcs` (in the SDK image), so no gUSA is left at all (`make gusa-scan`: COW string / exception_ptr counts, std::thread start, unwinder registry). Lifts the last C++ rules in docs/cpp-apps.md
- ch8 byte 15 = power state? (02 -> 03 -> 00 as the car went off on 28.10 and 30.10; on 30.10 with HMI "OnOff new system state is 5" and the unit running on for minutes): switch the ignition off on purpose during hello7, note the second, and compare
- name the new ch8 button bits (src/hello7/README.md, 25.10): one press each, written down in order
- option 2a (hardware H.264): live streaming works (04.11, docs/phone-mirroring.md 7.10). Next: phone MediaCodec H.264 (Baseline, 720x432, steady 25 fps with repeat-previous-frame, frame size capped to S); AOA frames for codec config + H.264 frames; move hello9's stream_server/stream_plan/vout/iom into src/common and feed it from audi_stream_receiver; header (avcC) built from the phone's SPS/PPS
- option 2a: measure end-to-end latency with one photo (frame number on both screens); find whether the ~9-frame io-media queue depends on S or fps; 10+ min session; HMI media/reverse camera while streaming; drop c10g from hello9's default list
- (low priority since CS-MP4 streams, 04.11) re-check the fMP4 explanation in io-media's CDemuxMp4 (the "no trun handler" one came from mp4_parser.so, which the trackplayer path doesn't seem to use)
- hello5 next run: does the 1 s re-show (`screen_set_reshow`) bring hello5 back after MEDIA in step `t`, and without a visible fight/flicker with the Media screen? If so, use it in hello7/hello9 too
- how MMI displays back camera video?
- czy da sie czytać info z CAN?
- shut down MMI safely without ignition
// wmsprobe.h - read-only look at the Bluetooth stack's stream devices
// (/dev/wms, PSSBSSProcess's MediaRMService, docs/bt-audio-path.md §2).
//
// Lists /dev/wms and, for player1, does what io-media's BT_DEVCHECK does
// before each Bluetooth play: open read-only, devctl 0x4004051e
// (connected?) and 0x40100520 (state, codec, rate), close. Nothing is
// read from the stream and nothing is written.
//
// It runs in a child process (hello12 <sd> @wms) with a deadline, so a
// server that never replies cannot hang hello12 itself.
#ifndef HELLO12_WMSPROBE_H
#define HELLO12_WMSPROBE_H

// The child's work; its lines go to log_msg. Returns the exit code.
int wms_probe_main();

#endif

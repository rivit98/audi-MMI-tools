// aoa.h - USB side of audi_stream_receiver: an Android Open Accessory host on the
// MMI's io-usb stack (libusbdi).
//
// A manager thread owns everything:
//   1. usbd_connect() to io-usb with an all-wildcard filter (retried every
//      2 s while io-usb is not there).
//   2. Any new device, 1 s after it appears (the media launcher probes it
//      first): AOA handshake on its control pipe (GET_PROTOCOL 51, six
//      identification strings 52, START 53). A device that refuses
//      GET_PROTOCOL (a USB stick, a hub, an iPod) is left alone; any other
//      failure is retried up to 5 times with a growing delay.
//   3. The phone re-enumerates as 18d1:2d00..2d05. Its accessory interface
//      (class/subclass ff/ff) gets a bulk IN and OUT pipe; IN is read
//      continuously with asynchronous usbd_io(), frames go to model.h
//      (HELLO: the peer) and, reassembled by vasm.h, hwvideo.h (H.264). The
//      MMI's VIDEO_CAPS goes out with its HELLO; VIDEO_ACKS are sent from
//      the manager thread.
//   4. Removal or any transfer error tears the connection down and goes
//      back to waiting. Nothing in here ever exits the process.
//
// Callbacks from io-usb only queue events; all blocking work runs on the
// manager thread.
#ifndef AUDI_STREAM_AOA_H
#define AUDI_STREAM_AOA_H

#include "proto.h"

namespace aoa {

// System log snapshots per run, each <sd>/var/usb_slog_<time>.txt (log_stamp)
constexpr int kSlogSnapshots = 6;

// false if the manager thread could not start. sd_path: where a failed
// handshake or a removal saves a system log snapshot (var/usb_slog_<time>.txt).
// screen_w x screen_h: the screen, for VIDEO_CAPS.
bool start(const char *sd_path, unsigned screen_w, unsigned screen_h);
void stop();    // tear down and join; bounded by quit's hard alarm

// A frame is done (any thread): goes out in the next VIDEO_ACKS, sent at
// once (a bad or cut-short frame).
void ack(proto::PicStatus status);
// The same, sent with a full batch or on the manager's next tick, whichever
// comes first (ack_batch.h).
void ack_later(proto::PicStatus status);

// Something for the manager to send (any thread).
void wake();

// The system log now, in the background, into var/usb_slog_<time>.txt (up to
// kSlogSnapshots per run); logs "usb: slog snapshot <n> (<why>)". Any thread.
// While frames come, the manager also saves it every 60 s into
// var/usb_slog_periodic_<time>.txt.
void slog_snapshot(const char *why);

} // namespace aoa

#endif

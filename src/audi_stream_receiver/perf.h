// perf.h - where each picture's time goes on the MMI, cheaply.
//
// The SD card is slow and log_msg() fsyncs every line, so nothing here
// writes from the threads that move pictures. They hand a record to a RAM
// ring (one mutex, a copy); a writer thread at a lower priority turns
// the records into
//
//   audi_stream_receiver.log   one "perf:" summary line per kWindowMs with
//                     pictures (rate, avg/max of every stage, size, decode
//                     and phone time, VFB bytes, main-loop
//                     busy, process CPU, what the JPEG cap held back), a
//                     "perf: stream" line for the whole stream when it
//                     stops, and rate-limited warnings: "perf: HIGH main
//                     load" (main loop >= kHighBusyPct busy over a window),
//                     "perf: JPEG cap hit" (every picture of a window held
//                     back: the phone sends faster than the cap)
//   audi_stream_perf.csv   one line per picture (columns in perf.cpp), written
//                     in batches of kBatch lines (or when the stream
//                     stops / kBatchMs passes), one fsync per batch
//
// A full ring (writer starved) drops records and counts them.
#ifndef AUDI_STREAM_PERF_H
#define AUDI_STREAM_PERF_H

#include "proto.h"

namespace perf {

constexpr unsigned kWindowMs = 10000;
constexpr unsigned kBatch = 128;
constexpr unsigned kBatchMs = 30000;
constexpr unsigned kHighBusyPct = 80;

// One picture. Times in microseconds (log_us()).
struct Pic {
	unsigned t_ms = 0;        // log_ms() when shown / dropped / refused
	unsigned id = 0, bytes = 0, w = 0, h = 0;
	unsigned phone_ms = 0;    // the phone's scale + encode (VIDEO_START), 0 = not sent
	proto::PicStatus status = proto::PicStatus::Shown;
	unsigned rx_us = 0;       // VIDEO_START in -> last VIDEO_DATA in (USB)
	unsigned wait_us = 0;     // complete -> taken by the main loop
	unsigned decode_us = 0;
	unsigned render_us = 0;   // ui::render() into the RAM canvas
	unsigned copy_us = 0;     // canvas -> VFB (compare + copy)
	unsigned flip_us = 0;     // lm_update_vfb + lm_get_vfb
	unsigned vfb_bytes = 0, vfb_rows = 0;
	bool     vfb_full = false;  // a full VFB copy
	unsigned latency_us = 0;  // VIDEO_START in -> on screen
	unsigned cap_us = 0;      // complete, held back undecoded by the JPEG cap
	unsigned hold_us = 0;     // on screen, its ack held back by the cap
};

// Opens <out_dir>/audi_stream_perf.csv (out_dir = <sd>/var), starts the
// writer. Logs the clock's resolution.
void init(const char *out_dir);
// Writes out what is left and ends the writer; bounded by quit.c's hard alarm.
void shutdown();
// Any thread; never blocks on the card.
void add(const Pic &p);
// Main loop: time it spent waiting for work (for "busy %").
void idle(unsigned us);
// Streaming stopped: the writer writes out what it has and logs the
// stream's totals. Any thread.
void flush();

} // namespace perf

#endif

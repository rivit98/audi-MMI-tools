/*
 * perf.h - where each picture's time goes on the MMI, cheaply.
 *
 * The SD card is slow and log_msg() fsyncs every line, so nothing here
 * writes from the threads that move pictures. They hand a record to a RAM
 * ring (one mutex, a memcpy); a writer thread at a lower priority turns
 * the records into
 *
 *   usb_tx_test.log   one "perf:" summary line per PERF_WINDOW_MS with
 *                     pictures (rate, avg/max of every stage, VFB bytes,
 *                     main-loop busy, process CPU), and a "perf: stream"
 *                     line for the whole stream when it stops
 *   usb_tx_perf.csv   one line per picture (columns in perf.c), written
 *                     in batches of PERF_BATCH lines (or when the stream
 *                     stops / PERF_BATCH_MS passes), one fsync per batch
 *
 * A full ring (writer starved) drops records and counts them.
 */
#ifndef USBTX_PERF_H
#define USBTX_PERF_H

#define PERF_WINDOW_MS 10000
#define PERF_BATCH     128
#define PERF_BATCH_MS  30000

/* One picture. Times in microseconds (log_us()). */
struct perf_pic {
	unsigned t_ms;        /* log_ms() when shown / dropped / refused */
	unsigned id, bytes, w, h;
	int      status;      /* PROTO_VIDEO_SHOWN / _BAD / _DROPPED */
	unsigned rx_us;       /* VIDEO_START in -> last VIDEO_DATA in (USB) */
	unsigned wait_us;     /* complete -> taken by the main loop */
	unsigned decode_us;
	unsigned render_us;   /* ui_render() into the RAM canvas */
	unsigned copy_us;     /* canvas -> VFB (compare + copy) */
	unsigned flip_us;     /* lm_update_vfb + lm_get_vfb */
	unsigned vfb_bytes, vfb_rows;
	int      vfb_full;    /* a full VFB copy */
	unsigned latency_us;  /* VIDEO_START in -> on screen */
};

/* Opens <sd>/usb_tx_perf.csv, starts the writer. Logs the clock's
 * resolution. */
void perf_init(const char *sd_path);
/* Any thread; never blocks on the card. */
void perf_add(const struct perf_pic *p);
/* Main loop: time it spent waiting for work (for "busy %"). */
void perf_idle(unsigned us);
/* Streaming stopped: the writer writes out what it has and logs the
 * stream's totals. Any thread. */
void perf_flush(void);

#endif

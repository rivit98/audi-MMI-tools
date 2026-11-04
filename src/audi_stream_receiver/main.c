/*
 * audi_stream_receiver - receive a slow video stream (the phone's screen, ~2 JPEG
 * pictures a second) from an Android phone over USB (Android Open
 * Accessory) and show it on the MMI screen. (It also took text messages
 * until 2026-10-25; README.md says why they went.)
 *
 *   phone app (android/)  --USB, AOA bulk-->  this  -->  screen + SD log
 *
 * Runs until the SD card is pulled or it gets SIGTERM/INT/HUP. A phone
 * can be plugged, unplugged and replugged any number of times: every drop
 * goes back to "waiting for phone" (aoa.c). The screen is a full-screen
 * layer-manager displayable drawn by the CPU (screen.c, the lmprobe path),
 * redrawn whenever the model changes or a picture arrives, and once a
 * second for the clock. Pictures are decoded here, on the main thread
 * (video.c); the USB threads only collect them.
 *
 * Writes <sd>/audi_stream_receiver.log: states, every USBDI result, every message
 * but the picture data, a "perf:" summary every 10 s of streaming and a
 * "cpu:" line every 10 s (own and unit load, per thread; cpumon.h);
 * <sd>/audi_stream_perf.csv: one line of timings per picture (perf.h).
 * The previous run's files are kept as *.prev* / *_prev_*.
 *
 * usage: audi_stream_receiver <sd_path> [copy=changed|full]
 *   copy=changed  (default) write only the changed part of each row into
 *                 the VFB (screen_set_partial)
 *   copy=full     the whole screen every time, as before 2026-10-28 (to
 *                 compare)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aoa.h"
#include "cpumon.h"
#include "log.h"
#include "model.h"
#include "perf.h"
#include "quit.h"
#include "screen.h"
#include "ui.h"
#include "video.h"

#define CPU_WINDOW_MS 10000
/* "HIGH" in the cpu: line and on screen: own load over a window. 30.10:
 * ~27 % while streaming 3 fps; the unit has navigation etc. to run. */
#define CPU_WARN_PCT  50

/* old -> new, replacing new. */
static void keep(const char *dir, const char *old_name, const char *new_name)
{
	char from[256], to[256];

	snprintf(from, sizeof(from), "%s/%s", dir, old_name);
	snprintf(to, sizeof(to), "%s/%s", dir, new_name);
	if (access(from, F_OK) != 0)
		return;
	unlink(to);
	rename(from, to);
}

/* A card often goes through several runs before it is read. On 13.10 a
 * second run overwrote the log of the one that saw the phone and left that
 * run's slog snapshots behind, looking like this run's. */
static void keep_previous_run(const char *sd_path)
{
	char var[256], name[32], prev[32];
	int n;

	keep(sd_path, "audi_stream_receiver.log", "audi_stream_receiver.prev.log");
	keep(sd_path, "audi_stream_perf.csv", "audi_stream_perf.prev.csv");
	snprintf(var, sizeof(var), "%s/var", sd_path);
	for (n = 1; n <= AOA_SLOG_SNAPSHOTS; n++) {
		snprintf(name, sizeof(name), "usb_slog_%d.txt", n);
		snprintf(prev, sizeof(prev), "usb_slog_prev_%d.txt", n);
		keep(var, name, prev);
	}
}

int main(int argc, char *argv[])
{
	const char *sd_path;
	char path[256];
	unsigned last_version = ~0u, last_second = ~0u, last_present = 0;
	int shown, fresh, i, partial = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [copy=changed|full]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	for (i = 2; i < argc; i++)
		if (strcmp(argv[i], "copy=full") == 0)
			partial = 0;
	keep_previous_run(sd_path);
	snprintf(path, sizeof(path), "%s/audi_stream_receiver.log", sd_path);
	if (log_open(path) != 0)
		fprintf(stderr, "audi_stream_receiver: cannot open %s, logging to stdout only\n", path);
	log_msg("audi_stream_receiver start, pid=%d, copy=%s", getpid(), partial ? "changed" : "full");
	quit_init(sd_path, 0);            /* no watchdog: runs until SD pull / signal */
	cpumon_name_thread("main");
	cpumon_start(CPU_WINDOW_MS, CPU_WARN_PCT);
	model_init();
	perf_init(sd_path);
	screen_set_partial(partial);

	shown = screen_open() == 0;
	if (!shown)
		log_msg("FAIL screen; running headless (log only)");
	if (shown)
		video_init(screen_canvas()->w, screen_canvas()->h);
	else
		video_init(800, 480);
	aoa_start(sd_path);

	while (quit_poll() == QUIT_NONE) {
		unsigned v, s, t0;

		fresh = video_poll();
		v = model_version();
		s = log_ms() / 1000;
		/* The clock alone redraws at most once a second: a full screen copy
		 * into the VFB costs ~160 ms, and pictures redraw anyway. */
		if (shown && (fresh || v != last_version ||
		              (s != last_second && log_ms() - last_present >= 1000))) {
			struct video_present p;
			const struct screen_stats *st;

			last_version = v;
			last_second = s;
			t0 = log_us();
			ui_render(screen_canvas(), sd_path);
			p.render_us = log_us() - t0;
			screen_present();
			last_present = log_ms();
			st = screen_last_stats();
			p.copy_us = st->copy_us;
			p.flip_us = st->flip_us;
			p.vfb_bytes = st->bytes;
			p.vfb_rows = st->rows;
			p.vfb_full = st->full;
			if (fresh)
				video_presented(&p);
		} else if (fresh) {
			video_presented(NULL);        /* headless: still ack it */
		}
		/* Straight on if the next picture is already here (the phone keeps
		 * two in flight); else until one comes, or 50 ms for the rest. */
		t0 = log_us();
		video_wait(50);
		perf_idle(log_us() - t0);
	}

	aoa_stop();
	screen_close();
	cpumon_stop();
	log_msg("audi_stream_receiver exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

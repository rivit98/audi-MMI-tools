/*
 * usb_tx_test - receive text and a slow video stream (the phone's screen,
 * ~2 JPEG pictures a second) from an Android phone over USB (Android Open
 * Accessory) and show them on the MMI screen.
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
 * Writes <sd>/usb_tx_test.log: states, every USBDI result, every message.
 * The previous run's log and slog snapshots are kept as *.prev* / *_prev_*.
 *
 * usage: usb_tx_test <sd_path>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aoa.h"
#include "log.h"
#include "model.h"
#include "quit.h"
#include "screen.h"
#include "ui.h"
#include "video.h"

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

	keep(sd_path, "usb_tx_test.log", "usb_tx_test.prev.log");
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
	int shown, fresh;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path>\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	keep_previous_run(sd_path);
	snprintf(path, sizeof(path), "%s/usb_tx_test.log", sd_path);
	if (log_open(path) != 0)
		fprintf(stderr, "usb_tx_test: cannot open %s, logging to stdout only\n", path);
	log_msg("usb_tx_test start, pid=%d", getpid());
	quit_init(sd_path, 0);            /* no watchdog: runs until SD pull / signal */
	model_init();

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
		/* The clock alone redraws at most once a second: a screen copy into
		 * the VFB costs ~160 ms, and pictures redraw anyway. */
		if (shown && (fresh || v != last_version ||
		              (s != last_second && log_ms() - last_present >= 1000))) {
			last_version = v;
			last_second = s;
			t0 = log_ms();
			ui_render(screen_canvas(), sd_path);
			screen_present();
			last_present = log_ms();
			if (fresh)
				video_presented(last_present - t0);
		} else if (fresh) {
			video_presented(0);           /* headless: still ack it */
		}
		usleep(fresh ? 10 * 1000 : 50 * 1000);
	}

	aoa_stop();
	screen_close();
	log_msg("usb_tx_test exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

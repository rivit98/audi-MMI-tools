/*
 * usb_tx_test - receive text from an Android phone over USB (Android Open
 * Accessory) and show it on the MMI screen.
 *
 *   phone app (android/)  --USB, AOA bulk-->  this  -->  screen + SD log
 *
 * Runs until the SD card is pulled or it gets SIGTERM/INT/HUP. A phone
 * can be plugged, unplugged and replugged any number of times: every drop
 * goes back to "waiting for phone" (aoa.c). The screen is a full-screen
 * layer-manager displayable drawn by the CPU (screen.c, the lmprobe path),
 * redrawn whenever the model changes and once a second for the clock.
 *
 * Writes <sd>/usb_tx_test.log: states, every USBDI result, every message.
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

int main(int argc, char *argv[])
{
	const char *sd_path;
	char path[256];
	unsigned last_version = ~0u, last_second = ~0u;
	int shown;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path>\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	snprintf(path, sizeof(path), "%s/usb_tx_test.log", sd_path);
	if (log_open(path) != 0)
		fprintf(stderr, "usb_tx_test: cannot open %s, logging to stdout only\n", path);
	log_msg("usb_tx_test start, pid=%d", getpid());
	quit_init(sd_path, 0);            /* no watchdog: runs until SD pull / signal */
	model_init();

	shown = screen_open() == 0;
	if (!shown)
		log_msg("FAIL screen; running headless (log only)");
	aoa_start(sd_path);

	while (quit_poll() == QUIT_NONE) {
		unsigned v = model_version(), s = log_ms() / 1000;

		if (shown && (v != last_version || s != last_second)) {
			last_version = v;
			last_second = s;
			ui_render(screen_canvas(), sd_path);
			screen_present();
		}
		usleep(50 * 1000);
	}

	aoa_stop();
	screen_close();
	log_msg("usb_tx_test exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

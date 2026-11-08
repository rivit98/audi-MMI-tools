// audi_stream_receiver - receive the phone's screen from an Android phone
// over USB (Android Open Accessory) and show it on the MMI screen: as JPEG
// pictures, a few a second, decoded here; or as an
// H.264 stream decoded by the unit's DM642 (hwvideo.h). (It has no text
// messages; README.md says why.)
//
//   phone app (android/)  --USB, AOA bulk-->  this  -->  screen + SD log
//
// Runs until the SD card is pulled or it gets SIGTERM/INT/HUP. A phone
// can be plugged, unplugged and replugged any number of times: every drop
// goes back to "waiting for phone" (aoa.cpp). The screen is a full-screen
// layer-manager displayable drawn by the CPU (screen.c, the lmprobe path),
// redrawn whenever the model changes or a picture arrives, and once a
// second for the clock. Pictures are decoded here, on the main thread
// (video.cpp); the USB threads only collect them. While an H.264 stream
// plays, the screen is the video's: the CPU-drawn one is closed, and
// opened again when the stream ends.
//
// Writes <sd>/audi_stream_receiver.log: states, every USBDI result, every message
// but the picture data, a "perf:" summary every 10 s of streaming and a
// "cpu:" line every 10 s (own and unit load, per thread; cpumon.h);
// <sd>/audi_stream_perf.csv: one line of timings per picture (perf.h).
// The previous run's files are kept as *.prev* / *_prev_*.
//
// usage: audi_stream_receiver <sd_path> [copy=changed|full]
//   copy=changed  (default) write only the changed part of each row into
//                 the VFB (screen_set_partial)
//   copy=full     the whole screen every time (to compare)
#include <cstdio>
#include <cstring>

#include <unistd.h>

extern "C" {
#include "cpumon.h"
#include "log.h"
#include "quit.h"
#include "screen.h"
}

#include "aoa.h"
#include "hwvideo.h"
#include "model.h"
#include "perf.h"
#include "ui.h"
#include "video.h"

namespace {

constexpr unsigned kCpuWindowMs = 10000;
// "HIGH" in the cpu: line and on screen: own load over a window: about
// 27 % while streaming 3 fps; the unit has navigation etc. to run.
constexpr unsigned kCpuWarnPct = 50;

// old -> new, replacing new.
void keep(const char *dir, const char *old_name, const char *new_name)
{
	char from[256], to[256];

	std::snprintf(from, sizeof(from), "%s/%s", dir, old_name);
	std::snprintf(to, sizeof(to), "%s/%s", dir, new_name);
	if (access(from, F_OK) != 0)
		return;
	unlink(to);
	rename(from, to);
}

// A card often goes through several runs before it is read. Without this a
// second run overwrites the log of the one that saw the phone and leaves that
// run's slog snapshots behind, looking like this run's.
void keep_previous_run(const char *sd_path)
{
	char var[256], name[32], prev[32];

	keep(sd_path, "audi_stream_receiver.log", "audi_stream_receiver.prev.log");
	keep(sd_path, "audi_stream_perf.csv", "audi_stream_perf.prev.csv");
	std::snprintf(var, sizeof(var), "%s/var", sd_path);
	for (int n = 1; n <= aoa::kSlogSnapshots; n++) {
		std::snprintf(name, sizeof(name), "usb_slog_%d.txt", n);
		std::snprintf(prev, sizeof(prev), "usb_slog_prev_%d.txt", n);
		keep(var, name, prev);
	}
}

} // namespace

int main(int argc, char *argv[])
{
	char path[256];
	unsigned last_version = ~0u, last_second = ~0u, last_present = 0;
	bool partial = true;

	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path> [copy=changed|full]\n", argv[0]);
		return 2;
	}
	const char *sd_path = argv[1];
	for (int i = 2; i < argc; i++)
		if (std::strcmp(argv[i], "copy=full") == 0)
			partial = false;
	keep_previous_run(sd_path);
	std::snprintf(path, sizeof(path), "%s/audi_stream_receiver.log", sd_path);
	if (log_open(path) != 0)
		std::fprintf(stderr, "audi_stream_receiver: cannot open %s, logging to stdout only\n", path);
	log_msg("audi_stream_receiver start, pid=%d, copy=%s", getpid(), partial ? "changed" : "full");
	quit_init(sd_path, 0);            // no watchdog: runs until SD pull / signal
	cpumon_name_thread("main");
	cpumon_start(kCpuWindowMs, kCpuWarnPct);
	perf::init(sd_path);
	screen_set_partial(partial);

	bool shown = screen_open() == 0;
	if (!shown)
		log_msg("FAIL screen; running headless (log only)");
	if (shown)
		video::init(screen_canvas()->w, screen_canvas()->h);
	else
		video::init(800, 480);
	bool screen_up = shown;
	hwvideo::start();
	aoa::start(sd_path);

	while (quit_poll() == QUIT_NONE) {
		// the H.264 stream's video takes the screen (vout replaces what
		// the layer manager shows); ours comes back when it ends
		if (shown && hwvideo::active() && screen_up) {
			log_msg("screen: closed for the H.264 video");
			screen_close();
			screen_up = false;
		} else if (shown && !hwvideo::active() && !screen_up) {
			log_msg("screen: back after the H.264 video");
			screen_up = screen_open() == 0;
			if (!screen_up) {
				log_msg("FAIL screen: could not open it again; headless from now on");
				shown = false;
			}
			last_version = ~0u;               // redraw
		}

		bool fresh = video::poll();
		unsigned v = model::version();
		unsigned s = log_ms() / 1000;
		// The clock alone redraws at most once a second: a full screen copy
		// into the VFB costs ~160 ms, and pictures redraw anyway.
		if (screen_up && (fresh || v != last_version ||
		              (s != last_second && log_ms() - last_present >= 1000))) {
			video::Present p;

			last_version = v;
			last_second = s;
			unsigned t0 = log_us();
			ui::render(screen_canvas(), sd_path);
			p.render_us = log_us() - t0;
			screen_present();
			last_present = log_ms();
			const screen_stats *st = screen_last_stats();
			p.copy_us = st->copy_us;
			p.flip_us = st->flip_us;
			p.vfb_bytes = st->bytes;
			p.vfb_rows = st->rows;
			p.vfb_full = st->full != 0;
			if (fresh)
				video::presented(&p);
		} else if (fresh) {
			video::presented(nullptr);        // headless: still ack it
		}
		// Straight on if the next picture is already here (the phone keeps
		// two in flight); else until one comes, or 50 ms for the rest.
		unsigned t0 = log_us();
		video::wait(50);
		perf::idle(log_us() - t0);
	}

	aoa::stop();
	hwvideo::shutdown();
	if (screen_up)
		screen_close();
	cpumon_stop();
	perf::shutdown();
	log_msg("audi_stream_receiver exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

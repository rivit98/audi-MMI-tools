// audi_stream_receiver - receive the phone's screen from an Android phone
// over USB (Android Open Accessory) and show it on the MMI screen as an
// H.264 stream decoded by the unit's DM642 (hwvideo.h). (It has no text
// messages; README.md says why.)
//
//   phone app (android/)  --USB, AOA bulk-->  this  -->  screen + SD log
//
// Runs until the SD card is pulled or it gets SIGTERM/INT/HUP. A phone
// can be plugged, unplugged and replugged any number of times: every drop
// goes back to "waiting for phone" (aoa.cpp). The screen is a full-screen
// layer-manager displayable drawn by the CPU (screen.c, the lmprobe path),
// redrawn whenever the model changes, and once a second for the clock.
// While the H.264 stream plays, the screen is the video's: the CPU-drawn
// one is closed, and opened again when the stream ends.
//
// Writes <sd>/audi_stream_receiver.log: states, every USBDI result, every message
// but the video data, "h264:" stream lines (hwvideo.h) and a
// "cpu:" line every 10 s (own and unit load, per thread; cpumon.h).
// The previous run's log is kept as *.prev.*; the slog snapshots
// carry the time in their names (log_stamp), so runs never overwrite them.
//
// usage: audi_stream_receiver <sd_path>
// Only the changed part of each row goes into the VFB (screen_set_partial).
#include <cstdio>

#include <unistd.h>

#include "camera.h"
#include "camera_log.h"

extern "C" {
#include "cpumon.h"
#include "log.h"
#include "slogmark.h"
#include "quit.h"
#include "screen.h"
}

#include "aoa.h"
#include "hwvideo.h"
#include "model.h"
#include "ui.h"

namespace {

constexpr unsigned kCpuWindowMs = 10000;
// "HIGH" in the cpu: line and on screen: own load over a window; the unit
// has navigation etc. to run.
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
// second run overwrites the log of the one that saw the phone.
void keep_previous_run(const char *sd_path)
{
	const char *var = var_dir(sd_path);

	keep(var, "audi_stream_receiver.log", "audi_stream_receiver.prev.log");
}

} // namespace

int main(int argc, char *argv[])
{
	char path[256];
	unsigned last_version = ~0u, last_second = ~0u, last_present = 0;

	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <sd_path>\n", argv[0]);
		return 2;
	}
	const char *sd_path = argv[1];
	keep_previous_run(sd_path);
	std::snprintf(path, sizeof(path), "%s/audi_stream_receiver.log", var_dir(sd_path));
	if (log_open(path) != 0)
		std::fprintf(stderr, "audi_stream_receiver: cannot open %s, logging to stdout only\n", path);
	log_msg("audi_stream_receiver start, pid=%d", getpid());
	quit_init(sd_path, 0);            // no watchdog: runs until SD pull / signal
	if (slogmark_init("audi_stream_receiver") == 0)
		slogmark("start, pid %d", (int)getpid());
	cpumon_name_thread("main");
	cpumon_start(kCpuWindowMs, kCpuWarnPct);
	screen_set_partial(1);

	camera::start();
	// our first show would cover the rear camera
	while (camera::active() && quit_poll() == QUIT_NONE)
		usleep(100 * 1000);

	bool shown = screen_open() == 0;
	if (!shown)
		log_msg("FAIL screen; running headless (log only)");
	bool screen_up = shown;
	camera::Takeover takeover;
	hwvideo::start();
	if (shown)
		aoa::start(sd_path, (unsigned)screen_canvas()->w, (unsigned)screen_canvas()->h);
	else
		aoa::start(sd_path, 800, 480);

	while (quit_poll() == QUIT_NONE) {
		// the H.264 stream's video takes the screen (vout replaces what
		// the layer manager shows); ours comes back when it ends
		// (not during the rear camera: both of these show something)
		if (shown && hwvideo::active() && screen_up && !camera::active()) {
			log_msg("screen: closed for the H.264 video");
			screen_close();
			screen_up = false;
		} else if (shown && !hwvideo::active() && !screen_up && camera::clear()) {
			log_msg("screen: back after the H.264 video");
			screen_up = screen_open() == 0;
			if (!screen_up) {
				log_msg("FAIL screen: could not open it again; headless from now on");
				shown = false;
			}
			last_version = ~0u;               // redraw
		}

		// The HMI's list replaces ours when the rear camera ends, too.
		using Action = camera::Takeover::Action;
		switch (takeover.tick(screen_up, camera::active(), camera::clear(), log_ms())) {
		case Action::Retake:
			log_msg("screen: the rear camera has gone: back");
			screen_show_again();
			break;
		case Action::ShowAgain:
			screen_show_again();
			break;
		case Action::GiveBack:
			log_msg("screen: the rear camera started as the screen came back: the camera goes first");
			screen_give_back_to_camera();
			break;
		case Action::None:
			break;
		}

		unsigned v = model::version();
		unsigned s = log_ms() / 1000;
		// The clock alone redraws at most once a second: a full screen copy
		// into the VFB costs ~160 ms.
		if (screen_up && (v != last_version ||
		              (s != last_second && log_ms() - last_present >= 1000))) {
			last_version = v;
			last_second = s;
			ui::render(screen_canvas(), var_dir(sd_path));
			screen_present();
			last_present = log_ms();
		}
		usleep(50 * 1000);
	}

	aoa::stop();
	hwvideo::shutdown();
	if (screen_up) {
		if (camera::active())
			screen_no_show_on_close();
		screen_close();
	}
	camera::stop();
	cpumon_stop();
	slogmark("exit (stop reason: %s)", quit_name(quit_poll()));
	log_msg("audi_stream_receiver exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

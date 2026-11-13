// camera.cpp - see camera.h.
#include "camera.h"
#include "types.h"

#include "camera_log.h"

extern "C" {
#include "log.h"
}

#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace camera {

namespace {

constexpr unsigned kPollMs = 250;

int fd = -1;
Tracker tracker;                        // the thread, or start() before it
std::atomic<bool> is_active{false};
std::atomic<bool> has_ended{false};
std::atomic<unsigned> ended_at{0};
std::atomic<bool> quit{false};
std::thread thread;
bool thread_started = false;
int buffer[4096];                       // the biggest event is 258 ints

// Everything new in the slog. false: read failed for good.
bool drain()
{
	for (;;) {
		isize n = read(fd, buffer, sizeof(buffer));
		if (n < 0) {
			if (errno == EAGAIN)
				return true;
			if (errno == EINTR)
				continue;
			log_msg("camera: /dev/slog read: %s: no camera detection from now on",
			        std::strerror(errno));
			return false;
		}
		if (n == 0)
			return true;
		tracker.events(buffer, (usize)n / sizeof(int), log_ms());
		if (tracker.ended())
			ended_at = tracker.ended_ms();
		has_ended = tracker.ended();
		if (tracker.active() != is_active.load()) {
			is_active = tracker.active();
			log_msg("camera: %s", tracker.active() ? "up: the HMI owns VCAP 0"
			                                        : "off: VCAP 0 is free");
		}
	}
}

void run()
{
	while (!quit) {
		if (!drain()) {
			is_active = false;
			has_ended = false;
			return;
		}
		usleep(kPollMs * 1000);
	}
}

} // namespace

void start()
{
	if (thread_started)
		return;
	fd = open("/dev/slog", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		log_msg("camera: /dev/slog: %s: no camera detection", std::strerror(errno));
		return;
	}
	if (!drain()) {
		close(fd);
		fd = -1;
		return;
	}
	log_msg("camera: slog read, the camera is %s", is_active ? "UP" : "off");
	quit = false;
	try {
		thread = std::thread(run);
		thread_started = true;
	} catch (const std::system_error &) {
		log_msg("camera: no thread: no camera detection");
		is_active = false;
		has_ended = false;
		close(fd);
		fd = -1;
	}
}

void stop()
{
	if (!thread_started)
		return;
	quit = true;
	thread.join();
	thread_started = false;
	close(fd);
	fd = -1;
}

bool active()
{
	return is_active;
}

bool clear()
{
	if (is_active)
		return false;
	return !has_ended || log_ms() - ended_at.load() >= kSettleMs;
}

} // namespace camera

// quit.cpp - stop requests and the cleanup (see quit.h).
#include "quit.h"

extern "C" {
#include "ch8.h"
#include "ipc.h"
#include "log.h"
}

#include <atomic>
#include <cstring>
#include <mutex>

#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

namespace quit {

namespace {

std::atomic<Reason> reason{Reason::None};
Config cfg;
unsigned start_ms, sd_checked_ms;
sigset_t sigs;
bool key_held;

// 0 not run, 1 running, 2 done: the cleanup runs once
std::mutex cleanup_mu;
int cleanup_state;

// true if the caller now owns the cleanup (it hadn't started)
bool take_cleanup()
{
	std::lock_guard<std::mutex> lock(cleanup_mu);
	if (cleanup_state != 0)
		return false;
	cleanup_state = 1;
	return true;
}

void *exit_later(void *arg)
{
	struct timespec ts = {kHardCleanupS, 0};
	nanosleep(&ts, nullptr);
	log_msg("quit: hard cleanup took over %u s: _exit", kHardCleanupS);
	_exit((int)(isize)arg);
	return nullptr;
}

[[noreturn]] void hard_exit(int code)
{
	pthread_t tid;
	if (cfg.cleanup && take_cleanup()) {
		log_msg("quit: hard cleanup");
		if (pthread_create(&tid, nullptr, exit_later, (void *)(isize)code) == 0)
			cfg.cleanup(true);
	}
	_exit(code);
}

void *signal_thread(void *)
{
	for (;;) {
		int sig;
		if (sigwait(&sigs, &sig) != 0)
			continue;
		if (reason == Reason::None) {
			log_msg("got signal %d", sig);
			request(sig == SIGALRM ? Reason::Watchdog : Reason::Signal);
			continue;
		}
		// already stopping: a second signal, or the teardown hung
		log_msg("signal %d while stopping (%s): _exit", sig, name(reason));
		hard_exit(sig == SIGALRM ? 124 : 128 + sig);
	}
	return nullptr;
}

// ch8: a press of the stop key (it wasn't held in the message before)
void on_ch8(int, u32, const u8 *buf, int n, void *)
{
	if (n <= CH8_POWER_BYTE || buf[CH8_KIND_BYTE] == CH8_KIND_TOUCH)
		return;          // a touch pad message says nothing about buttons
	bool held = ch8_held(buf, n, cfg.stop_key) != 0;
	if (held && !key_held)
		request(Reason::Key);
	key_held = held;
}

} // namespace

const char *name(Reason r)
{
	switch (r) {
	case Reason::None:     return "none";
	case Reason::Done:     return "done";
	case Reason::Timer:    return "timer";
	case Reason::Signal:   return "signal";
	case Reason::SdCard:   return "SD card removed";
	case Reason::Key:      return "key";
	case Reason::Watchdog: return "watchdog";
	}
	return "?";
}

void request(Reason r)
{
	Reason none = Reason::None;
	if (r == Reason::None || !reason.compare_exchange_strong(none, r))
		return;
	alarm(kHardS);           // the teardown gets kHardS seconds
	log_msg("stop requested: %s", name(r));
}

Reason poll()
{
	if (reason != Reason::None)
		return reason;
	unsigned now = log_ms();
	if (cfg.run_ms != 0 && now - start_ms >= cfg.run_ms) {
		request(Reason::Timer);
	} else if (cfg.sd_path != nullptr && now - sd_checked_ms >= 1000) {
		sd_checked_ms = now;
		if (access(cfg.sd_path, F_OK) != 0)
			request(Reason::SdCard);
	}
	return reason;
}

Reason finish()
{
	request(Reason::Done);
	if (cfg.cleanup && take_cleanup()) {
		cfg.cleanup(false);
		std::lock_guard<std::mutex> lock(cleanup_mu);
		cleanup_state = 2;
	}
	alarm(0);
	return reason;
}

bool start(const Config &c)
{
	cfg = c;
	start_ms = sd_checked_ms = log_ms();

	// Block the stop signals in every thread (threads inherit the mask);
	// signal_thread picks them up with sigwait().
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
	pthread_t tid;
	int rc = pthread_create(&tid, nullptr, signal_thread, nullptr);
	if (rc != 0) {
		log_msg("quit: signal thread: %s; signals/watchdog kill without cleanup",
		        std::strerror(rc));
		pthread_sigmask(SIG_UNBLOCK, &sigs, nullptr);
	} else {
		pthread_detach(tid);
	}
	alarm(cfg.watchdog_s);
	if (cfg.stop_key != 0 && ipc_subscribe(8, on_ch8, nullptr, "quit") < 0)
		cfg.stop_key = 0;
	log_msg("quit: stop on signals%s%s%s, run %u ms, watchdog %u s, stop key %s, cleanup %s",
	        cfg.sd_path != nullptr ? ", SD removal (" : "",
	        cfg.sd_path != nullptr ? cfg.sd_path : "",
	        cfg.sd_path != nullptr ? ")" : "",
	        cfg.run_ms, cfg.watchdog_s,
	        cfg.stop_key != 0 ? "yes" : "none", cfg.cleanup ? "yes" : "none");
	return rc == 0;
}

} // namespace quit

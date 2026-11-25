/*
 * quit.c - stop requests and the cleanup (see quit.h).
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ch8.h"
#include "ipc.h"
#include "log.h"
#include "quit.h"

static volatile int quit_reason;
static struct quit_config cfg;
static unsigned start_ms, sd_checked_ms;
static sigset_t quit_sigs;
static int key_held;

/* 0 not run, 1 running, 2 done; taken under cleanup_mu so it runs once */
static pthread_mutex_t cleanup_mu = PTHREAD_MUTEX_INITIALIZER;
static int cleanup_state;

const char *quit_name(int reason)
{
	switch (reason) {
	case QUIT_NONE:     return "none";
	case QUIT_DONE:     return "done";
	case QUIT_TIMER:    return "timer";
	case QUIT_SIGNAL:   return "signal";
	case QUIT_SDCARD:   return "SD card removed";
	case QUIT_KEY:      return "key";
	case QUIT_WATCHDOG: return "watchdog";
	}
	return "?";
}

void quit_request(int reason)
{
	if (quit_reason != QUIT_NONE || reason == QUIT_NONE)
		return;
	quit_reason = reason;
	alarm(QUIT_HARD_S);      /* the teardown gets QUIT_HARD_S seconds */
	log_msg("stop requested: %s", quit_name(reason));
}

int quit_poll(void)
{
	unsigned now = log_ms();

	if (quit_reason != QUIT_NONE)
		return quit_reason;
	if (cfg.run_ms != 0 && now - start_ms >= cfg.run_ms)
		quit_request(QUIT_TIMER);
	else if (cfg.sd_path != NULL && now - sd_checked_ms >= 1000) {
		sd_checked_ms = now;
		if (access(cfg.sd_path, F_OK) != 0)
			quit_request(QUIT_SDCARD);
	}
	return quit_reason;
}

/* 1 if the caller now owns the cleanup (it hadn't started). */
static int take_cleanup(void)
{
	int mine;

	pthread_mutex_lock(&cleanup_mu);
	mine = cleanup_state == 0;
	if (mine)
		cleanup_state = 1;
	pthread_mutex_unlock(&cleanup_mu);
	return mine;
}

int quit_finish(void)
{
	quit_request(QUIT_DONE);
	if (cfg.cleanup != NULL && take_cleanup()) {
		cfg.cleanup(0, cfg.arg);
		pthread_mutex_lock(&cleanup_mu);
		cleanup_state = 2;
		pthread_mutex_unlock(&cleanup_mu);
	}
	alarm(0);
	return quit_reason;
}

static void *exit_later(void *arg)
{
	struct timespec ts = { QUIT_HARD_CLEANUP_S, 0 };

	nanosleep(&ts, NULL);
	log_msg("quit: hard cleanup took over %d s: _exit", QUIT_HARD_CLEANUP_S);
	_exit((int)(isize)arg);
	return NULL;
}

static void hard_exit(int code)
{
	pthread_t tid;

	if (cfg.cleanup != NULL && take_cleanup()) {
		log_msg("quit: hard cleanup");
		if (pthread_create(&tid, NULL, exit_later, (void *)(isize)code) == 0)
			cfg.cleanup(1, cfg.arg);
	}
	_exit(code);
}

static void *signal_thread(void *arg)
{
	int sig;

	(void)arg;
	for (;;) {
		if (sigwait(&quit_sigs, &sig) != 0)
			continue;
		if (quit_reason == QUIT_NONE) {
			log_msg("got signal %d", sig);
			quit_request(sig == SIGALRM ? QUIT_WATCHDOG : QUIT_SIGNAL);
			continue;
		}
		/* already stopping: a second signal, or the teardown hung */
		log_msg("signal %d while stopping (%s): _exit", sig, quit_name(quit_reason));
		hard_exit(sig == SIGALRM ? 124 : 128 + sig);
	}
	return NULL;
}

/* ch8: a press of the stop key (it wasn't held in the message before). */
static void on_ch8(int ch, u32 ms, const u8 *buf, int n, void *arg)
{
	int held;

	(void)ch;
	(void)ms;
	(void)arg;
	if (n <= CH8_POWER_BYTE || buf[CH8_KIND_BYTE] == CH8_KIND_TOUCH)
		return;          /* a touch pad message says nothing about buttons */
	held = ch8_held(buf, n, cfg.stop_key);
	if (held && !key_held)
		quit_request(QUIT_KEY);
	key_held = held;
}

int quit_start(const struct quit_config *c)
{
	pthread_t tid;
	int rc;

	cfg = *c;
	start_ms = sd_checked_ms = log_ms();

	/* Block the stop signals in every thread (threads inherit the mask);
	 * signal_thread picks them up with sigwait(). */
	sigemptyset(&quit_sigs);
	sigaddset(&quit_sigs, SIGTERM);
	sigaddset(&quit_sigs, SIGINT);
	sigaddset(&quit_sigs, SIGHUP);
	sigaddset(&quit_sigs, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &quit_sigs, NULL);
	rc = pthread_create(&tid, NULL, signal_thread, NULL);
	if (rc != 0) {
		log_msg("quit: signal thread: %s; signals/watchdog kill without cleanup",
		        strerror(rc));
		pthread_sigmask(SIG_UNBLOCK, &quit_sigs, NULL);
	} else {
		pthread_detach(tid);
	}
	alarm(cfg.watchdog_s);
	if (cfg.stop_key != 0 && ipc_subscribe(8, on_ch8, NULL, "quit") < 0)
		cfg.stop_key = 0;
	log_msg("quit: stop on signals%s%s%s, run %u ms, watchdog %u s, stop key %s, cleanup %s",
	        cfg.sd_path != NULL ? ", SD removal (" : "",
	        cfg.sd_path != NULL ? cfg.sd_path : "",
	        cfg.sd_path != NULL ? ")" : "",
	        cfg.run_ms, cfg.watchdog_s,
	        cfg.stop_key != 0 ? "yes" : "none", cfg.cleanup != NULL ? "yes" : "none");
	return rc == 0 ? 0 : -1;
}

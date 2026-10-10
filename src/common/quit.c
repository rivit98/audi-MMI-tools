/*
 * quit.c - stop requests from timer, keys, signals, SD removal, watchdog
 * (see quit.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "quit.h"

#define KEY_DEVICE   "/dev/ipc/ch8"
#define KEY_LOG_MAX  30      /* log the first messages raw, to learn the protocol */

static volatile int quit_reason;
static const char *sd;
static unsigned sd_checked_ms;
static sigset_t quit_sigs;
static quit_key_cb key_cb;
static void *key_cb_arg;

const char *quit_name(int reason)
{
	switch (reason) {
	case QUIT_NONE:     return "none";
	case QUIT_TIMER:    return "timer";
	case QUIT_KEY:      return "key press";
	case QUIT_SIGNAL:   return "signal";
	case QUIT_SDCARD:   return "SD card removed";
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

	if (quit_reason == QUIT_NONE && sd != NULL && now - sd_checked_ms >= 1000) {
		sd_checked_ms = now;
		if (access(sd, F_OK) != 0)
			quit_request(QUIT_SDCARD);
	}
	return quit_reason;
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
		_exit(sig == SIGALRM ? 124 : 128 + sig);
	}
	return NULL;
}

/* Blocking reads of the HMI's key channel, as qnx-audi does (no select()
 * support there). Byte 7/6 = rotary knob left/right, 5/4 = volume knob;
 * any other message is a button press. */
static void *key_thread(void *arg)
{
	int fd = (int)(intptr_t)arg, n, i, seen = 0;
	unsigned char buf[256];
	char hex[16 * 3 + 1];

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			log_msg("%s: read returned %d (%s), key thread ends", KEY_DEVICE, n,
			        n < 0 ? strerror(errno) : "EOF");
			break;
		}
		if (seen++ < KEY_LOG_MAX) {
			for (i = 0; i < n && i < 16; i++)
				snprintf(hex + i * 3, 4, "%02x ", buf[i]);
			hex[i * 3] = '\0';
			log_msg("%s: %d bytes: %s", KEY_DEVICE, n, hex);
		}
		if (key_cb != NULL)
			key_cb(buf, n, key_cb_arg);
		if (n >= 8 && (buf[4] | buf[5] | buf[6] | buf[7]) == 0)
			quit_request(QUIT_KEY);
	}
	close(fd);
	return NULL;
}

int quit_init(const char *sd_path, unsigned watchdog_s)
{
	return quit_init_ex(sd_path, watchdog_s, NULL, NULL);
}

int quit_init_ex(const char *sd_path, unsigned watchdog_s,
                 quit_key_cb cb, void *cb_arg)
{
	pthread_t tid;
	int fd, rc;

	sd = sd_path;
	sd_checked_ms = log_ms();
	key_cb = cb;
	key_cb_arg = cb_arg;

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
	alarm(watchdog_s);

	fd = open(KEY_DEVICE, O_RDONLY);
	if (fd < 0) {
		log_msg("quit: open %s: %s; no key exit", KEY_DEVICE, strerror(errno));
	} else if ((errno = pthread_create(&tid, NULL, key_thread, (void *)(intptr_t)fd)) != 0) {
		log_msg("quit: key thread: %s; no key exit", strerror(errno));
		close(fd);
	} else {
		pthread_detach(tid);   /* stays blocked in read(); exit() ends it */
	}
	log_msg("quit: watchdog %u s, stop on: timer, button (%s), SIGTERM/INT/HUP, "
	        "SD removal (%s)", watchdog_s, KEY_DEVICE, sd_path);
	return rc == 0 ? 0 : -1;
}

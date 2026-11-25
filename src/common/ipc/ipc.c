/*
 * ipc.c - one reader thread per /dev/ipc channel (see ipc.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ipc.h"
#include "log.h"

#define MAX_SUBS  16
#define READ_MAX  4096

struct sub {
	int id, ch;
	ipc_cb cb;
	void *arg;
	const char *name;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;  /* subs; held while calling them */
static struct sub subs[MAX_SUBS];
static int next_id = 1;
static const char *state[IPC_MAX_CH];                   /* NULL: not started */
static volatile int stop[IPC_MAX_CH];

static void set_state(int ch, const char *s)
{
	pthread_mutex_lock(&mu);
	state[ch] = s;
	pthread_mutex_unlock(&mu);
}

/* Drops channel ch's subscribers after ipc_stop(). Called with mu held. */
static void drop_locked(int ch)
{
	int i;

	for (i = 0; i < MAX_SUBS; i++)
		if (subs[i].id != 0 && subs[i].ch == ch)
			memset(&subs[i], 0, sizeof(subs[i]));
	state[ch] = "stopped";
}

static int fds[IPC_MAX_CH];

static void *reader(void *arg)
{
	int ch = (int)(isize)arg, fd = fds[ch], i, done;
	u8 buf[READ_MAX];
	isize n;

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR && !stop[ch])
			continue;
		if (n <= 0 && !stop[ch]) {
			log_msg("ipc: ch%d: read returned %d (%s): no more reads", ch, (int)n,
			        n < 0 ? strerror(errno) : "end");
			set_state(ch, "read failed");
			break;
		}
		pthread_mutex_lock(&mu);
		if (!stop[ch]) {
			u32 ms = log_ms();

			for (i = 0; i < MAX_SUBS; i++)
				if (subs[i].id != 0 && subs[i].ch == ch)
					subs[i].cb(ch, ms, buf, (int)n, subs[i].arg);
		}
		/* a callback may have stopped the channel (ipc_stop takes no lock) */
		done = stop[ch];
		if (done)
			drop_locked(ch);
		pthread_mutex_unlock(&mu);
		if (done)
			break;
	}
	close(fd);
	return NULL;
}

/* Opens the channel on the caller's thread, so a missing channel shows at
 * subscribe time, and hands the fd to a new reader. Called with mu held. */
static int start(int ch)
{
	pthread_t tid;
	char path[32];
	int rc;

	snprintf(path, sizeof(path), "/dev/ipc/ch%d", ch);
	/* close-on-exec: a child that inherits the channel and never reads it
	 * stalls it for every reader after 32 messages */
	fds[ch] = open(path, O_RDONLY | O_CLOEXEC);
	if (fds[ch] < 0) {
		log_msg("ipc: open %s: %s", path, strerror(errno));
		state[ch] = "no open";
		return -1;
	}
	rc = pthread_create(&tid, NULL, reader, (void *)(isize)ch);
	if (rc != 0) {
		log_msg("ipc: %s: no reader thread: %s", path, strerror(rc));
		state[ch] = "no thread";
		close(fds[ch]);
		return -1;
	}
	pthread_detach(tid);   /* blocked in read() until the process ends */
	state[ch] = "reading";
	return 0;
}

int ipc_subscribe(int ch, ipc_cb cb, void *arg, const char *name)
{
	int i, id = -1;

	if (ch < 0 || ch >= IPC_MAX_CH || cb == NULL)
		return -1;
	pthread_mutex_lock(&mu);
	if (state[ch] == NULL)
		start(ch);
	if (strcmp(state[ch], "reading") == 0 && !stop[ch]) {
		for (i = 0; i < MAX_SUBS; i++) {
			if (subs[i].id == 0) {
				subs[i].id = id = next_id++;
				subs[i].ch = ch;
				subs[i].cb = cb;
				subs[i].arg = arg;
				subs[i].name = name;
				break;
			}
		}
	}
	pthread_mutex_unlock(&mu);
	if (id < 0)
		log_msg("ipc: ch%d: %s not subscribed (%s)", ch, name ? name : "?",
		        state[ch] != NULL ? state[ch] : "?");
	else
		log_msg("ipc: ch%d: %s subscribed (#%d)", ch, name ? name : "?", id);
	return id;
}

void ipc_unsubscribe(int id)
{
	int i;

	if (id <= 0)
		return;
	pthread_mutex_lock(&mu);
	for (i = 0; i < MAX_SUBS; i++)
		if (subs[i].id == id)
			memset(&subs[i], 0, sizeof(subs[i]));
	pthread_mutex_unlock(&mu);
}

/* No lock: it may be called from a callback, on the reader thread. The
 * reader drops the subscribers after its current read. */
void ipc_stop(int ch, const char *why)
{
	if (ch < 0 || ch >= IPC_MAX_CH)
		return;
	log_msg("ipc: ch%d: stop reading (%s)", ch, why);
	stop[ch] = 1;
}

const char *ipc_state(int ch)
{
	const char *s;

	if (ch < 0 || ch >= IPC_MAX_CH)
		return "?";
	pthread_mutex_lock(&mu);
	s = state[ch] == NULL ? "not started" : stop[ch] ? "stopped" : state[ch];
	pthread_mutex_unlock(&mu);
	return s;
}

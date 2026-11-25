/*
 * ducklink.c - hello5's side of the step d helper (ducklink.h).
 *
 * QNX 6.3 refuses fork() in a threaded process, so the helper is spawn()ed
 * (posix_spawn on the host, for the tests). Only the two pipe ends and
 * /dev/null reach it: no screen, ch8, log or audio fd.
 */
#include "ducklink.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

#include "log.h"

static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000);
}

static void nap_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

void link_init(struct duck_link *l)
{
	memset(l, 0, sizeof(*l));
	l->to = l->from = -1;
}

static void cloexec(int fd)
{
	fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int link_start(struct duck_link *l, char *const argv[])
{
	int to[2], from[2], nul, err = 0;
	pid_t pid = -1;

	link_init(l);
	/* a write to a helper that died must not end hello5 */
	signal(SIGPIPE, SIG_IGN);
	if (pipe(to) != 0)
		return errno;
	if (pipe(from) != 0) {
		err = errno;
		close(to[0]);
		close(to[1]);
		return err;
	}
	/* the parent's ends stay out of every later child (proc.c's spawnvp) */
	cloexec(to[0]);
	cloexec(to[1]);
	cloexec(from[0]);
	cloexec(from[1]);
	nul = open("/dev/null", O_RDWR);
#ifdef __QNXNTO__
	{
		/* only these four fds reach the child (fd_count > 0); an empty
		 * signal mask (quit blocks SIGTERM in hello5 for its sigwait) */
		int fd_map[4] = { to[0], nul >= 0 ? nul : SPAWN_FDCLOSED,
		                  nul >= 0 ? nul : SPAWN_FDCLOSED, from[1] };
		struct inheritance inh;

		memset(&inh, 0, sizeof(inh));
		inh.flags = SPAWN_SETSIGMASK;
		sigemptyset(&inh.sigmask);
		pid = spawn(argv[0], 4, fd_map, &inh, argv, environ);
		if (pid < 0)
			err = errno;
	}
#else
	{
		posix_spawn_file_actions_t fa;
		posix_spawnattr_t at;
		sigset_t none;

		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, to[0], 0);
		if (nul >= 0) {
			posix_spawn_file_actions_adddup2(&fa, nul, 1);
			posix_spawn_file_actions_adddup2(&fa, nul, 2);
		}
		posix_spawn_file_actions_adddup2(&fa, from[1], DUCK_LINK_FD);
		posix_spawnattr_init(&at);
		sigemptyset(&none);
		posix_spawnattr_setsigmask(&at, &none);
		posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
		err = posix_spawn(&pid, argv[0], &fa, &at, argv, environ);
		posix_spawn_file_actions_destroy(&fa);
		posix_spawnattr_destroy(&at);
		if (err != 0)
			pid = -1;
	}
#endif
	close(to[0]);
	close(from[1]);
	if (nul >= 0)
		close(nul);
	if (pid < 0) {
		close(to[1]);
		close(from[0]);
		return err ? err : EIO;
	}
	l->pid = pid;
	l->to = to[1];
	l->from = from[0];
	log_msg("helper: started %s, pid %d", argv[0], (int)pid);
	return 0;
}

int link_parse(const char *line, unsigned *ms, const char **rest)
{
	static const struct {
		const char *word;
		int kind;
	} words[] = {
		{ "ok", LINK_OK }, { "err", LINK_ERR }, { "wait", -1 }, { "note", -2 },
	};
	unsigned i;

	*rest = line;
	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		usize n = strlen(words[i].word);

		if (strncmp(line, words[i].word, n) != 0 ||
		    (line[n] != '\0' && line[n] != ' '))
			continue;
		*rest = line[n] == ' ' ? line + n + 1 : line + n;
		if (words[i].kind == -1) {
			char *end;
			u32 v = (u32)strtoul(*rest, &end, 10);

			if (end == *rest)
				return -3;
			*ms = (unsigned)v;
			*rest = *end == ' ' ? end + 1 : end;
		}
		return words[i].kind;
	}
	return -3;
}

/* Kill a helper that missed its deadline. It may not die (REPLY-blocked on
 * a channel that ignores unblock): nothing here waits for that. */
static void give_up(struct duck_link *l, const char *why)
{
	log_msg("helper: %s: SIGKILL pid %d, carrying on without it", why,
	        (int)l->pid);
	kill(l->pid, SIGKILL);
	l->dead = 1;
}

/* The next complete line from buf into line; 1 if there was one. */
static int take_line(struct duck_link *l, char *line, usize size)
{
	char *nl = memchr(l->buf, '\n', l->len);
	usize n;

	if (nl == NULL) {
		if (l->len < sizeof(l->buf) - 1)
			return 0;
		nl = l->buf + l->len - 1;       /* overlong: cut it */
	}
	n = (usize)(nl - l->buf);
	if (n >= size)
		n = size - 1;
	memcpy(line, l->buf, n);
	line[n] = '\0';
	n = (usize)(nl - l->buf) + 1;
	memmove(l->buf, l->buf + n, l->len - n);
	l->len -= n;
	return 1;
}

int link_cmd(struct duck_link *l, const char *cmd, unsigned first_ms,
             char *reply, usize size, int (*stop)(void))
{
	char line[sizeof(l->buf)];
	unsigned deadline = now_ms() + first_ms + DUCK_LINK_SLACK_MS;
	usize len = strlen(cmd);

	snprintf(l->what, sizeof(l->what), "%s", cmd);
	l->what_ms = now_ms();
	l->deadline = deadline;

	if (size > 0)
		reply[0] = '\0';
	if (l->pid <= 0 || l->dead)
		return LINK_GONE;
	log_msg("helper <- %s", cmd);
	if (write(l->to, cmd, len) != (isize)len || write(l->to, "\n", 1) != 1) {
		log_msg("helper: write \"%s\": %s", cmd, strerror(errno));
		l->dead = 1;
		return LINK_GONE;
	}
	for (;;) {
		unsigned ms;
		const char *rest;
		int kind;

		while (take_line(l, line, sizeof(line))) {
			log_msg("helper -> %s", line);
			kind = link_parse(line, &ms, &rest);
			if (kind == -1) {
				deadline = now_ms() + ms + DUCK_LINK_SLACK_MS;
				snprintf(l->what, sizeof(l->what), "%s", rest);
				l->what_ms = now_ms();
				l->deadline = deadline;
			} else if (kind == -2)
				snprintf(l->note, sizeof(l->note), "%s", rest);
			else if (kind == LINK_OK || kind == LINK_ERR) {
				if (size > 0)
					snprintf(reply, size, "%s", rest);
				return kind;
			}
		}
		if (l->progress != NULL)
			l->progress(l, now_ms());
		if (stop != NULL && stop())
			return LINK_STOP;
		{
			unsigned t = now_ms();
			struct timeval tv;
			fd_set rd;
			isize n;
			int r;

			if ((int)(deadline - t) <= 0) {
				char why[sizeof(line) + 32];

				snprintf(why, sizeof(why), "no answer to \"%s\"", cmd);
				give_up(l, why);
				return LINK_TIMEOUT;
			}
			ms = deadline - t;
			if (ms > DUCK_LINK_POLL_MS)
				ms = DUCK_LINK_POLL_MS;
			tv.tv_sec = 0;
			tv.tv_usec = (long)ms * 1000L;
			FD_ZERO(&rd);
			FD_SET(l->from, &rd);
			r = select(l->from + 1, &rd, NULL, NULL, &tv);
			if (r < 0 && errno != EINTR) {
				log_msg("helper: select: %s", strerror(errno));
				give_up(l, "select failed");
				return LINK_TIMEOUT;
			}
			if (r <= 0)
				continue;
			n = read(l->from, l->buf + l->len, sizeof(l->buf) - 1 - l->len);
			if (n <= 0) {
				log_msg("helper: pid %d closed its answers (%s)", (int)l->pid,
				        n == 0 ? "exited" : strerror(errno));
				l->dead = 1;
				return LINK_GONE;
			}
			l->len += (usize)n;
		}
	}
}

/* waitpid with WNOHANG for up to ms; 1 if reaped. */
static int reap(pid_t pid, unsigned ms)
{
	unsigned end = now_ms() + ms;
	int st;

	for (;;) {
		pid_t r = waitpid(pid, &st, WNOHANG);

		if (r == pid || (r < 0 && errno == ECHILD))
			return 1;
		if ((int)(end - now_ms()) <= 0)
			return 0;
		nap_ms(50);
	}
}

int link_end(struct duck_link *l, unsigned grace_ms)
{
	int gone;

	if (l->pid <= 0) {
		link_init(l);
		return 1;
	}
	if (!l->dead)
		(void)!write(l->to, "quit\n", 5);
	close(l->to);           /* EOF: quit, also for a helper that missed it */
	close(l->from);
	gone = reap(l->pid, l->dead ? 0 : grace_ms);
	if (!gone) {
		kill(l->pid, SIGKILL);
		gone = reap(l->pid, 500);
	}
	if (gone)
		log_msg("helper: pid %d ended", (int)l->pid);
	else
		log_msg("helper: pid %d does not end even after SIGKILL (blocked in a "
		        "send to the MMI): left behind, it holds no screen", (int)l->pid);
	link_init(l);
	return gone;
}

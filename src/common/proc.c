#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <process.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define POLL_MS 50

pid_t proc_start(const char *out_path, char *const argv[])
{
	int fd, saved1, saved2, err;
	pid_t pid;

	fd = open(out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return -1;
	fflush(stdout);
	fflush(stderr);
	saved1 = dup(1);
	saved2 = dup(2);
	dup2(fd, 1);
	dup2(fd, 2);
	pid = spawnvp(P_NOWAIT, argv[0], argv);
	err = errno;
	dup2(saved1, 1);
	dup2(saved2, 2);
	close(saved1);
	close(saved2);
	close(fd);
	errno = err;
	return pid;
}

int proc_wait(pid_t pid, unsigned timeout_ms, int *status)
{
	unsigned waited = 0;

	for (;;) {
		pid_t r = waitpid(pid, status, WNOHANG);

		if (r == pid)
			return 0;
		if (r < 0 && errno != EINTR) {
			*status = 0;   /* not our child any more: treat as gone */
			return 0;
		}
		if (waited >= timeout_ms)
			return -1;
		usleep(POLL_MS * 1000);
		waited += POLL_MS;
	}
}

int proc_stop(pid_t pid)
{
	int status = 0;

	if (proc_wait(pid, 0, &status) == 0)
		return status;
	kill(pid, SIGTERM);
	if (proc_wait(pid, 2000, &status) == 0)
		return status;
	kill(pid, SIGKILL);
	if (proc_wait(pid, 2000, &status) == 0)
		return status;
	return -1;
}

int proc_shell(const char *out_path, unsigned timeout_s, const char *cmd)
{
	char *argv[] = { "ksh", "-c", (char *)cmd, NULL };
	int status = 0, timed_out = 0;
	FILE *f;
	pid_t pid;

	f = fopen(out_path, "a");
	if (f != NULL) {
		fprintf(f, "\n### $ %s\n", cmd);
		fclose(f);
	}
	pid = proc_start(out_path, argv);
	if (pid < 0) {
		f = fopen(out_path, "a");
		if (f != NULL) {
			fprintf(f, "### could not start: %s\n", strerror(errno));
			fclose(f);
		}
		return -1;
	}
	if (proc_wait(pid, timeout_s * 1000, &status) != 0) {
		timed_out = 1;
		status = proc_stop(pid);
	}
	f = fopen(out_path, "a");
	if (f != NULL) {
		fprintf(f, "### %s%s\n", proc_status_str(status),
		        timed_out ? " (stopped after timeout)" : "");
		fclose(f);
	}
	return status;
}

const char *proc_status_str(int status)
{
	static char buf[32];

	if (status == -1)
		return "did not exit";
	if (WIFEXITED(status))
		snprintf(buf, sizeof(buf), "exit %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		snprintf(buf, sizeof(buf), "signal %d", WTERMSIG(status));
	else
		snprintf(buf, sizeof(buf), "status 0x%x", status);
	return buf;
}

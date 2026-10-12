/*
 * proc.h - run the unit's own tools from a multithreaded app.
 *
 * QNX 6.3 refuses fork() in a process with more than one thread, and
 * quit.c starts threads, so children are started with spawnvp(P_NOWAIT).
 * Their stdout + stderr go to a file: fds 1 and 2 are pointed at it for
 * the duration of the spawn call (a child inherits every fd without
 * FD_CLOEXEC) and restored straight after.
 */
#ifndef HELLO6_PROC_H
#define HELLO6_PROC_H

#include <sys/types.h>

/* Start argv[0] (searched in PATH if it has no '/'), output appended to
 * out_path. Returns the pid, or -1 (errno set). */
pid_t proc_start(const char *out_path, char *const argv[]);

/* Wait up to timeout_ms for pid to exit. Returns 0 and the wait status in
 * *status if it did, -1 if it is still running. */
int proc_wait(pid_t pid, unsigned timeout_ms, int *status);

/* SIGTERM, up to 2 s grace, then SIGKILL; always reaps. Returns the wait
 * status. */
int proc_stop(pid_t pid);

/* `ksh -c cmd` with a "### $ cmd" header and a "### exit ..." trailer in
 * out_path, stopped after timeout_s. Returns the wait status, or -1 if it
 * could not be started. */
int proc_shell(const char *out_path, unsigned timeout_s, const char *cmd);

/* Human-readable wait status: "exit 0", "signal 9", "timeout". */
const char *proc_status_str(int status);

#endif

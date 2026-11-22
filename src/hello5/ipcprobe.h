/*
 * ipcprobe.h - hello5 step p: who MMI3GApplication's audio management talks
 * to, read from the kernel without sending anything to anyone.
 *
 * SGAudioManagement is not in the servicebroker, but MMI3GNavigation (a
 * child process of MMI3GApplication) calls it. This step records the
 * evidence for how: for MMI3GApplication, MMI3GNavigation and the
 * servicebroker it lists
 *  - every connection (fd and side channel): coid -> nd, pid (+ name),
 *    chid, scoid (ConnectServerInfo, as `pidin fd` does), and
 *  - every thread's state and what it is blocked on: RECEIVE on which of
 *    its own chids, SEND/REPLY to which pid/chid (DCMD_PROC_TIDSTATUS).
 * So the log shows which chid of MMI3GApplication the child (and the
 * broker) are connected to, and which of its threads receive on it.
 * The unit's own `pidin -p <pid>` (threads, fd, arg) output goes next to
 * it, each call bounded, as a cross-check.
 *
 * Full listing: <out_dir>/hello5_ipc.txt and hello5_pidin.txt; a summary
 * in the log ("step p: ..." lines).
 *
 * The three pids come from /proc first (each pid's name through
 * /proc/<pid>/as, as pidin does). Whatever /proc does not give comes from
 * the unit's sloginfo ("pid N: application X started"), then from
 * `pidin arg` (pidscan.c), their output kept in hello5_sloginfo.txt and
 * hello5_pidin_arg.txt. The walk's failure counts and errnos, the uid and
 * the source of each pid go into the log ("procs: ..." lines), once per
 * run.
 */
#ifndef HELLO5_IPCPROBE_H
#define HELLO5_IPCPROBE_H

#include <sys/types.h>

/* Runs the probe, writes the files into out_dir. 0 if both MMI3G
 * processes were found, -1 if not. */
int ipc_probe(const char *out_dir);

/* Worst-case run time in ms (the bounded pidin calls, the pid lookup
 * included), for the watchdog. */
unsigned ipc_probe_max_ms(void);

/* Worst-case run time in ms of the pid lookup alone (sloginfo, pidin). */
unsigned ipc_find_max_ms(void);

/* MMI3GApplication's pid, and the channels of it to send HBDSI to, best
 * first: the ones MMI3GNavigation holds a plain (non-side) connection to,
 * then its side-channel ones, then the channels MMI3GApplication's own
 * threads receive on (those need /proc). *n_nav: how many of the list's
 * first entries the navigation is connected to. out_dir: where the pid
 * lookup's tool output goes. Returns how many chids were written, or -1 if
 * the process is not running. Logs what it found ("step d: ..." lines). */
int ipc_app_channels(const char *out_dir, pid_t *app_pid, int *chids,
                     unsigned max, unsigned *n_nav);

/* Is `from` connected to pid/chid? 1 yes, 0 no. Used to tell whether a
 * service accepted a ConnectRequest: the server answers every request,
 * but it only connects back to the client's channels when it found the
 * service. */
int ipc_connected_to(pid_t from, pid_t to_pid, int to_chid);

#endif

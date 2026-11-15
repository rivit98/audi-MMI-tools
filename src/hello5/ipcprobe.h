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
 */
#ifndef HELLO5_IPCPROBE_H
#define HELLO5_IPCPROBE_H

/* Runs the probe, writes the files into out_dir. 0 if both MMI3G
 * processes were found, -1 if not. */
int ipc_probe(const char *out_dir);

/* Worst-case run time in ms (the bounded pidin calls), for the watchdog. */
unsigned ipc_probe_max_ms(void);

#endif

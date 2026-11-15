/*
 * svcscan.h - find a service's HBDSI server id in MMI3GApplication's
 * memory, read-only, from outside the process.
 *
 * SGAudioManagement is not in the servicebroker: MMI3GApplication serves
 * it to its own child processes, which address it by a server id that
 * only exists inside those processes (docs/hbdsi.md section 4). The id
 * is needed to send a ConnectRequest, and the registry that holds it
 * sits in MMI3GApplication's heap, next to the service's role name
 * ("AudioManagementDevCtrl").
 *
 * So: enumerate the process's mapped segments (/proc/<pid>/as,
 * DCMD_PROC_MAPINFO), find the role name in them, and dump the words
 * around every place that holds the name or a pointer to it. The words
 * that look like a component id - low half a subsystem number, high half
 * a small instance number, as the bootstrap's component table is built
 * (MMI3GApplication 0x0002.0x0027 = AudioManagementDevCtrl) - are
 * returned as candidates, best first.
 *
 * Nothing is written to the process and nothing is sent to it. The whole
 * dump goes to a file for later reading; only the candidate ids come
 * back.
 */
#ifndef HELLO5_SVCSCAN_H
#define HELLO5_SVCSCAN_H

#include <sys/types.h>

#include "svcid.h"
#include "types.h"

struct svc_result {
	u32      ids[SVC_MAX_IDS];      /* candidate server ids, best first */
	unsigned nids;
	unsigned segments;              /* mapped segments of the process */
	unsigned name_hits;             /* places holding the name itself */
	unsigned ptr_hits;              /* places holding a pointer to it */
	u64      bytes;                 /* memory read */
	unsigned ms;
	int      error;                 /* errno if the memory was unreadable */
};

/* Scans pid's memory for name, writes the report to out_path and fills
 * *res. 0 if the memory could be read (even with no hit), -1 if not. */
int svc_scan(pid_t pid, const char *name, const char *out_path,
             struct svc_result *res);

/* Worst-case run time in ms, for the watchdog. */
unsigned svc_scan_max_ms(void);

#endif

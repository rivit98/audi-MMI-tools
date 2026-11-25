/*
 * ipc.h - the IOC's message channels /dev/ipc/chN, read once per process
 * and handed to any number of observers.
 *
 * dev-ipc gives every reader its own copy of a channel, but a reader that
 * stops reading stalls the channel for every other reader, the HMI
 * included (docs/vehicle-data.md §2.2). So each channel has one reader
 * thread here, started by its first subscriber, and it keeps reading until
 * the process ends, with or without subscribers. Never open a channel for
 * reading anywhere else.
 *
 * Callbacks run on the channel's reader thread with the subscriber lock
 * held: keep them short, never block, never subscribe or unsubscribe from
 * inside one (that deadlocks). ipc_unsubscribe() returns once no callback
 * of that id is running, so its arg may be freed after it.
 */
#ifndef COMMON_IPC_IPC_H
#define COMMON_IPC_IPC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_CH4  "/dev/ipc/ch4"   /* IOC <-> MMI3GApplication: MOST telegrams, BAP (camera/ipc4.h) */
#define IPC_CH5  "/dev/ipc/ch5"   /* vehicle-signal records (docs/vehicle-data.md §3) */
#define IPC_CH8  "/dev/ipc/ch8"   /* knobs and buttons (ch8.h, buttons/README.md) */

#define IPC_MAX_CH  16            /* channels 0..15 */

/* One read() of channel ch: buf[0..n), ms = log_ms() when it returned. */
typedef void (*ipc_cb)(int ch, u32 ms, const u8 *buf, int n, void *arg);

/* Subscribe to channel ch (/dev/ipc/ch<ch>); the first subscriber opens it
 * and starts its reader. name: for the log, a literal (NULL: "#<id>").
 * Returns the id (> 0), or -1 if the channel can't be opened, its reader
 * can't start, or it was stopped. */
int  ipc_subscribe(int ch, ipc_cb cb, void *arg, const char *name);
void ipc_unsubscribe(int id);

/* Stop reading channel ch for good (e.g. camera's gap guard: dev-ipc
 * shares rather than copies the channel, so our reads would take telegrams
 * from MMI3GApplication). The reader closes it after its current read. */
void ipc_stop(int ch, const char *why);

/* "not started", "reading", "no open", "no thread", "read failed", "stopped". */
const char *ipc_state(int ch);

#ifdef __cplusplus
}
#endif

#endif

/*
 * hbdsi.h - a minimal HBDSI client: attach to a service interface through
 * the servicebroker, connect to its server, send DataRequests. The wire
 * format is hbdsi_wire.h; docs/hbdsi.md has the protocol.
 *
 * The server also connects back to us (ConnectRequest carries our two
 * receive channels) and may send DataResponses there. Two receiver
 * threads take every message on those channels and reply at once, so
 * the server's sender never blocks on us. Every message is logged.
 *
 * Every step logs what it did. Nothing here registers anything with the
 * broker; closing the broker fd (hbdsi_close) detaches us, which tells
 * the server we are gone.
 */
#ifndef COMMON_HBDSI_H
#define COMMON_HBDSI_H

#include <pthread.h>

#include "types.h"
#include "hbdsi_wire.h"

struct hbdsi {
	int broker_fd;
	struct hbdsi_attach_reply r;    /* from the attach */
	int chid[2];                    /* our receive channels */
	pthread_t rx[2];
	int rx_started[2];
	int coid;                       /* the server's receive channel */
	struct hbdsi_connect srv;       /* the server's reply to ConnectRequest */
	volatile unsigned rx_msgs[2];   /* messages received on chid[k] */
	volatile unsigned rx_pulses[2];
};

/* Read the broker's status text into buf (NUL-terminated). Bytes read, or
 * -1 (errno). */
int hbdsi_broker_list(char *buf, usize size);

/* Open the broker (once) and attach to name major.minor. Returns 0 and the
 * broker's status in *status (HBDSI_BROKER_OK fills c->r), or an errno if
 * the devctl itself failed. */
int hbdsi_attach(struct hbdsi *c, const char *name, u16 major, u16 minor,
                 int *status);

/* The other way to a service: no broker. A process serves the interfaces
 * it never registered to its own child processes, which address them by a
 * server id that exists only inside those processes (docs/hbdsi.md
 * section 4). Given the server's pid, the channel to send to and that id,
 * this fills in what the attach would have filled in. The client id
 * starts at 0: the server makes one up and the first message it sends
 * back carries it, which hbdsi then uses for the requests that follow. */
void hbdsi_target(struct hbdsi *c, u32 pid, u32 chid, u32 server_id);

/* After a successful attach: create our channels, start the receivers,
 * send ConnectRequest to the server (timeout_ms for its reply). 0, or an
 * errno. */
int hbdsi_connect(struct hbdsi *c, unsigned timeout_ms);

/* Send one DataRequest: request_id with n int32 arguments. The server
 * replies empty at once and handles the request afterwards. 0, or an
 * errno. */
int hbdsi_request(struct hbdsi *c, u32 request_id, const i32 *args,
                  unsigned nargs, unsigned timeout_ms);

/* Detach everything: connections, broker fd (= detach), channels,
 * receiver threads. Safe on a half-set-up c. */
void hbdsi_close(struct hbdsi *c);

/* Start state: call before anything else. */
void hbdsi_init(struct hbdsi *c);

#endif

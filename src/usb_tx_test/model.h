/*
 * model.h - what usb_tx_test shows: link state + the last received
 * messages. Written by the USB threads, read by the render loop; every
 * access goes through model_lock()/model_unlock() (or the setters, which
 * lock themselves).
 */
#ifndef USBTX_MODEL_H
#define USBTX_MODEL_H

#include "proto.h"

enum link_state {
	LINK_NO_STACK,   /* io-usb not reachable (yet); retrying */
	LINK_WAITING,    /* no phone */
	LINK_SWITCHING,  /* AOA start sent, waiting for the phone to come back */
	LINK_ACCESSORY,  /* phone in accessory mode, its app has not said hello */
	LINK_CONNECTED,  /* app said hello (or sent text) */
};

#define MODEL_MAX_MSGS   32
#define MODEL_DETAIL_LEN 120

struct model_msg {
	unsigned seq;
	unsigned ms;                         /* log_ms() when received */
	char     text[PROTO_MAX_PAYLOAD + 1];
};

struct model {
	enum link_state  state;
	char             detail[MODEL_DETAIL_LEN];
	unsigned         rx_count;
	unsigned         nmsgs;              /* valid entries, newest last */
	unsigned         first;              /* ring index of the oldest */
	struct model_msg msgs[MODEL_MAX_MSGS];
	unsigned         version;            /* bumped on every change */
};

void model_init(void);
struct model *model_lock(void);
void model_unlock(void);

/* detail may be NULL (cleared). Logs the transition. */
void model_set_state(enum link_state state, const char *detail);
void model_add_message(unsigned seq, const char *text, unsigned len);
unsigned model_version(void);

const char *link_state_name(enum link_state state);

/* i-th message, oldest first; m must be locked. */
static inline struct model_msg *model_msg_at(struct model *m, unsigned i)
{
	return &m->msgs[(m->first + i) % MODEL_MAX_MSGS];
}

#endif

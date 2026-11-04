/*
 * model.h - what audi_stream_receiver shows besides the picture: the link state.
 * Written by the USB threads, read by the render loop; every access goes
 * through model_lock()/model_unlock() (or the setters, which lock
 * themselves).
 */
#ifndef AUDI_STREAM_MODEL_H
#define AUDI_STREAM_MODEL_H

enum link_state {
	LINK_NO_STACK,   /* io-usb not reachable (yet); retrying */
	LINK_WAITING,    /* no phone */
	LINK_SWITCHING,  /* AOA start sent, waiting for the phone to come back */
	LINK_ACCESSORY,  /* phone in accessory mode, its app has not said hello */
	LINK_CONNECTED,  /* app said hello (or sent a picture) */
};

#define MODEL_DETAIL_LEN 120

struct model {
	enum link_state state;
	char            detail[MODEL_DETAIL_LEN];
	int             details;             /* status bar over a landscape picture */
	unsigned        version;             /* bumped on every change */
};

void model_init(void);
struct model *model_lock(void);
void model_unlock(void);

/* detail may be NULL (cleared). Logs the transition. */
void model_set_state(enum link_state state, const char *detail);
/* VIDEO_DETAILS from the phone. Logs a change. */
void model_set_details(int show);
unsigned model_version(void);

const char *link_state_name(enum link_state state);

#endif

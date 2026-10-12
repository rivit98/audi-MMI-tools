/*
 * model.c - shared app state (see model.h).
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "model.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct model model;

void model_init(void)
{
	pthread_mutex_lock(&lock);
	memset(&model, 0, sizeof(model));
	model.state = LINK_NO_STACK;
	pthread_mutex_unlock(&lock);
}

struct model *model_lock(void)
{
	pthread_mutex_lock(&lock);
	return &model;
}

void model_unlock(void)
{
	pthread_mutex_unlock(&lock);
}

const char *link_state_name(enum link_state state)
{
	switch (state) {
	case LINK_NO_STACK:  return "NO_STACK";
	case LINK_WAITING:   return "WAITING";
	case LINK_SWITCHING: return "SWITCHING";
	case LINK_ACCESSORY: return "ACCESSORY";
	case LINK_CONNECTED: return "CONNECTED";
	}
	return "?";
}

void model_set_state(enum link_state state, const char *detail)
{
	pthread_mutex_lock(&lock);
	if (model.state != state || strcmp(model.detail, detail ? detail : "") != 0) {
		log_msg("state %s -> %s%s%s", link_state_name(model.state),
		        link_state_name(state), detail ? ": " : "", detail ? detail : "");
		model.state = state;
		snprintf(model.detail, sizeof(model.detail), "%s", detail ? detail : "");
		model.version++;
	}
	pthread_mutex_unlock(&lock);
}

void model_add_message(unsigned seq, const char *text, unsigned len)
{
	struct model_msg *msg;

	if (len > PROTO_MAX_PAYLOAD)
		len = PROTO_MAX_PAYLOAD;
	pthread_mutex_lock(&lock);
	if (model.nmsgs == MODEL_MAX_MSGS) {          /* drop the oldest */
		model.first = (model.first + 1) % MODEL_MAX_MSGS;
		model.nmsgs--;
	}
	msg = model_msg_at(&model, model.nmsgs);
	msg->seq = seq;
	msg->ms = log_ms();
	memcpy(msg->text, text, len);
	msg->text[len] = 0;
	model.nmsgs++;
	model.rx_count++;
	model.version++;
	pthread_mutex_unlock(&lock);
}

unsigned model_version(void)
{
	unsigned v;

	pthread_mutex_lock(&lock);
	v = model.version;
	pthread_mutex_unlock(&lock);
	return v;
}

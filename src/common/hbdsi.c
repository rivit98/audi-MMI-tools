/*
 * hbdsi.c - see hbdsi.h.
 */
#include "types.h"
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/neutrino.h>
#include <sys/siginfo.h>

#include "hbdsi.h"
#include "log.h"

#define RX_BUF 4096

void hbdsi_init(struct hbdsi *c)
{
	memset(c, 0, sizeof(*c));
	c->broker_fd = -1;
	c->chid[0] = c->chid[1] = -1;
	c->coid = -1;
}

int hbdsi_broker_list(char *buf, usize size)
{
	int fd = open(HBDSI_BROKER_PATH, O_RDONLY);
	usize got = 0;
	isize n;

	if (size == 0)
		return -1;
	buf[0] = '\0';
	if (fd < 0)
		return -1;
	while (got + 1 < size && (n = read(fd, buf + got, size - 1 - got)) > 0)
		got += (usize)n;
	buf[got] = '\0';
	close(fd);
	return (int)got;
}

int hbdsi_attach(struct hbdsi *c, const char *name, u16 major, u16 minor,
                 int *status)
{
	union {
		struct hbdsi_attach_msg  in;
		struct hbdsi_attach_reply out;
	} m;
	int rc, flags;

	*status = -1;
	if (c->broker_fd < 0) {
		c->broker_fd = open(HBDSI_BROKER_PATH, O_RDWR);
		if (c->broker_fd < 0)
			c->broker_fd = open(HBDSI_BROKER_PATH, O_RDONLY);
		if (c->broker_fd < 0)
			return errno;
		flags = fcntl(c->broker_fd, F_GETFD);
		if (flags >= 0)
			fcntl(c->broker_fd, F_SETFD, flags | FD_CLOEXEC);
	}
	if (hbdsi_attach_msg_init(&m.in, name, major, minor) != 0)
		return ENAMETOOLONG;
	rc = devctl(c->broker_fd, (int)HBDSI_DCMD_ATTACH, &m, sizeof(m.in), status);
	if (rc != EOK)
		return rc;
	if (*status == HBDSI_BROKER_OK)
		c->r = m.out;
	return EOK;
}

void hbdsi_target(struct hbdsi *c, u32 pid, u32 chid, u32 server_id)
{
	memset(&c->r, 0, sizeof(c->r));
	c->r.nd = 0;            /* ND_LOCAL_NODE */
	c->r.pid = pid;
	c->r.chid = chid;
	c->r.server_id[0] = server_id;
}

/* One receiver per channel: take every message, reply at once (the
 * server's sender waits for that), log it. Pulses are logged too. Ends
 * when the channel is destroyed. */
struct rx_arg {
	struct hbdsi *c;
	int k;
};

static void *rx_thread(void *arg)
{
	struct rx_arg *a = arg;
	struct hbdsi *c = a->c;
	int k = a->k, rcvid;
	static u8 bufs[2][RX_BUF];
	u8 *buf = bufs[k];
	struct _msg_info info;

	for (;;) {
		rcvid = MsgReceive(c->chid[k], buf, RX_BUF, &info);
		if (rcvid < 0) {
			if (errno == EINTR)
				continue;
			log_msg("hbdsi rx %d: MsgReceive: %s, receiver ends", k, strerror(errno));
			return NULL;
		}
		if (rcvid == 0) {
			struct _pulse *p = (struct _pulse *)buf;

			c->rx_pulses[k]++;
			log_msg("hbdsi rx %d: pulse code %d value 0x%x", k, p->code,
			        (unsigned)p->value.sival_int);
			continue;
		}
		MsgReply(rcvid, EOK, NULL, 0);
		c->rx_msgs[k]++;
		if (info.msglen >= (int)sizeof(struct hbdsi_hdr)) {
			struct hbdsi_hdr h;
			char hex[16 * 3 + 1];
			int i, n = info.msglen - (int)sizeof(h);

			memcpy(&h, buf, sizeof(h));
			/* With no broker there is no client id until the server
			 * sends one: take it from the first message that has one. */
			if (c->r.client_id[0] == 0 && h.client_id[0] != 0) {
				c->r.client_id[0] = h.client_id[0];
				c->r.client_id[1] = h.client_id[1];
				log_msg("hbdsi rx %d: client id <%u.%u> from the server, used "
				        "for the requests that follow", k,
				        (unsigned)h.client_id[0], (unsigned)h.client_id[1]);
			}
			for (i = 0; i < n && i < 16 && (int)sizeof(h) + i < RX_BUF; i++)
				snprintf(hex + i * 3, 4, "%02x ", buf[sizeof(h) + i]);
			hex[i * 3] = '\0';
			log_msg("hbdsi rx %d: %d bytes from pid %d: type 0x%x cmd %u ids "
			        "<%u.%u> <%u.%u> len %u/%u payload: %s", k, info.msglen,
			        info.pid, (unsigned)h.type, (unsigned)h.cmd,
			        (unsigned)h.server_id[0], (unsigned)h.server_id[1],
			        (unsigned)h.client_id[0], (unsigned)h.client_id[1],
			        (unsigned)h.total_len, (unsigned)h.chunk_len, hex);
		} else {
			log_msg("hbdsi rx %d: %d bytes from pid %d (no HBDSI header)", k,
			        info.msglen, info.pid);
		}
	}
}

/* MsgSend with a deadline: a server that never replies must not hang us. */
static int send_timed(int coid, const void *msg, int len, void *reply, int rlen,
                      unsigned timeout_ms)
{
	struct sigevent ev;
	u64 ns = (u64)timeout_ms * 1000000ULL;

	SIGEV_UNBLOCK_INIT(&ev);
	TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
	             &ev, &ns, NULL);
	return MsgSend(coid, msg, len, reply, rlen);
}

int hbdsi_connect(struct hbdsi *c, unsigned timeout_ms)
{
	static struct rx_arg args[2];
	struct hbdsi_connect mine, reply;
	u8 msg[sizeof(struct hbdsi_hdr) + sizeof(struct hbdsi_connect)];
	int k, rc, len;

	for (k = 0; k < 2; k++) {
		c->chid[k] = ChannelCreate(0);
		if (c->chid[k] < 0)
			return errno;
		args[k].c = c;
		args[k].k = k;
		rc = pthread_create(&c->rx[k], NULL, rx_thread, &args[k]);
		if (rc != EOK)
			return rc;
		c->rx_started[k] = 1;
	}
	c->coid = ConnectAttach(c->r.nd, (pid_t)c->r.pid, (int)c->r.chid,
	                        _NTO_SIDE_CHANNEL, 0);
	if (c->coid < 0) {
		rc = errno;
		c->coid = -1;
		return rc;
	}
	mine.chid[0] = (u32)c->chid[0];
	mine.chid[1] = (u32)c->chid[1];
	mine.pid = (u32)getpid();
	mine.nd = 0;            /* ND_LOCAL_NODE: the server is on this node */
	len = hbdsi_build_connect(msg, sizeof(msg), &c->r, &mine);
	log_msg("hbdsi: ConnectRequest to pid %u chid %u: our chids %u/%u pid %u",
	        (unsigned)c->r.pid, (unsigned)c->r.chid, (unsigned)mine.chid[0],
	        (unsigned)mine.chid[1], (unsigned)mine.pid);
	memset(&reply, 0, sizeof(reply));
	if (send_timed(c->coid, msg, len, &reply, sizeof(reply), timeout_ms) < 0)
		return errno;
	c->srv = reply;
	log_msg("hbdsi: ConnectRequest reply: server chids %u/%u pid %u nd %u",
	        (unsigned)reply.chid[0], (unsigned)reply.chid[1],
	        (unsigned)reply.pid, (unsigned)reply.nd);
	return EOK;
}

int hbdsi_request(struct hbdsi *c, u32 request_id, const i32 *args,
                  unsigned nargs, unsigned timeout_ms)
{
	u8 msg[sizeof(struct hbdsi_hdr) + sizeof(struct hbdsi_request_base) + 16 * 4];
	int len;

	if (c->coid < 0 || nargs > 16)
		return EINVAL;
	len = hbdsi_build_request(msg, sizeof(msg), &c->r, request_id, args, nargs);
	if (len < 0)
		return EINVAL;
	if (send_timed(c->coid, msg, len, NULL, 0, timeout_ms) < 0)
		return errno;
	return EOK;
}

void hbdsi_close(struct hbdsi *c)
{
	int k;

	if (c->coid >= 0)
		ConnectDetach(c->coid);
	c->coid = -1;
	if (c->broker_fd >= 0)
		close(c->broker_fd);    /* the broker detaches our client id */
	c->broker_fd = -1;
	for (k = 0; k < 2; k++) {
		if (c->chid[k] >= 0)
			ChannelDestroy(c->chid[k]);     /* ends its receiver */
		c->chid[k] = -1;
		if (c->rx_started[k])
			pthread_join(c->rx[k], NULL);
		c->rx_started[k] = 0;
	}
}

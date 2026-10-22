/*
 * aoa.c - Android Open Accessory host on io-usb (see aoa.h).
 *
 * USBDI usage follows smartdevicelink's QNX transport (usb_handler.cc /
 * usb_connection.cc), which shipped on QNX head units: synchronous vendor
 * control transfers on the default pipe for the AOA handshake, asynchronous
 * bulk transfers for data, a short transfer reported as
 * USBD_STATUS_CMP_ERR | USBD_STATUS_DATA_UNDERRUN counting as success.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/usbdi.h>

#include "aoa.h"
#include "log.h"
#include "model.h"
#include "proto.h"
#include "vasm.h"
#include "video.h"

/* AOA protocol (source.android.com/docs/core/interaction/accessories/aoa) */
#define AOA_GET_PROTOCOL   51
#define AOA_SEND_STRING    52
#define AOA_START          53
#define AOA_VID            0x18D1
#define AOA_PID_MIN        0x2D00   /* 2d00 accessory, 2d01 +adb, 2d04/05 +audio */
#define AOA_PID_MAX        0x2D05

#define CTRL_TIMEOUT_MS    2000
#define CONNECT_RETRY_MS   2000
#define SWITCH_TIMEOUT_MS  10000
#define TEARDOWN_WAIT_MS   3000
#define REOPEN_DELAY_MS    500
#define REOPEN_MAX         5
#define HANDSHAKE_FIRST_MS 4000     /* after insertion: media launcher done (below) */
#define HANDSHAKE_DELAY_MS 1000     /* retry n waits n times this */
#define HANDSHAKE_TRIES    5
#define IN_BUF_SIZE        16384    /* Android accessory reads want >= 16 KB */
#define OUT_QUEUE          16
#define OUT_FRAME_MAX      (PROTO_HDR + 128)
#define MAX_DEVICES        16
#define MAX_EVENTS         32

/* --- events from io-usb callbacks -> manager thread ------------------ */

enum { EV_INSERT, EV_REMOVE };

struct event {
	int                    kind;
	usbd_device_instance_t inst;
};

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  qcond;
static struct event    queue[MAX_EVENTS];
static unsigned        qhead, qlen;
static int             stopping;      /* aoa_stop() called */
static int             kick;          /* manager has work besides events */
static pthread_t       mgr_tid;
static int             mgr_started;

static void slog_snapshot(const char *why);

static void post(int kind, const usbd_device_instance_t *inst)
{
	pthread_mutex_lock(&qlock);
	if (qlen < MAX_EVENTS) {
		struct event *ev = &queue[(qhead + qlen) % MAX_EVENTS];

		ev->kind = kind;
		ev->inst = *inst;
		qlen++;
	} else {
		log_msg("usb: event queue full, %s dropped",
		        kind == EV_INSERT ? "insert" : "remove");
	}
	pthread_cond_signal(&qcond);
	pthread_mutex_unlock(&qlock);
}

static void wake(void)
{
	pthread_mutex_lock(&qlock);
	kick = 1;
	pthread_cond_signal(&qcond);
	pthread_mutex_unlock(&qlock);
}

static void cb_insertion(struct usbd_connection *c, usbd_device_instance_t *inst)
{
	(void)c;
	post(EV_INSERT, inst);
}

static void cb_removal(struct usbd_connection *c, usbd_device_instance_t *inst)
{
	(void)c;
	post(EV_REMOVE, inst);
}

/* --- the accessory connection ----------------------------------------- */

struct link {
	struct usbd_device *dev;
	struct usbd_pipe   *in, *out;
	struct usbd_urb    *in_urb, *out_urb;
	uint8_t            *in_buf, *out_buf;    /* usbd_alloc()ed */
	struct proto_dec    dec;
	struct vasm         vasm;                /* the picture being received */

	pthread_mutex_t     lock;                /* out queue + flags below */
	uint8_t             outq[OUT_QUEUE][OUT_FRAME_MAX];
	unsigned            outq_len[OUT_QUEUE];
	unsigned            outq_head, outq_n;
	int                 in_pending, out_pending;
	int                 closing, broken;
	uint32_t            fail_status;         /* URB status that broke it */
	int                 hello_seen;
	int                 rx_frames;
};

static int urb_ok(struct usbd_urb *urb, uint32_t *len)
{
	uint32_t status = 0;
	int rc;

	*len = 0;
	rc = usbd_urb_status(urb, &status, len);
	if (rc != EOK && rc != EIO)
		return 0;
	return (status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP ||
	       status == (USBD_STATUS_CMP_ERR | USBD_STATUS_DATA_UNDERRUN);
}

static uint32_t urb_status(struct usbd_urb *urb)
{
	uint32_t status = 0, len = 0;

	usbd_urb_status(urb, &status, &len);
	return status;
}

static void out_done(struct usbd_urb *urb, struct usbd_pipe *pipe, void *arg);
static void in_done(struct usbd_urb *urb, struct usbd_pipe *pipe, void *arg);

/* l->lock held. Starts the next queued frame if nothing is in flight. */
static void out_kick(struct link *l)
{
	unsigned n;
	int rc;

	if (l->out_pending || l->outq_n == 0 || l->closing || l->broken)
		return;
	n = l->outq_len[l->outq_head];
	memcpy(l->out_buf, l->outq[l->outq_head], n);
	usbd_setup_bulk(l->out_urb, URB_DIR_OUT, l->out_buf, n);
	l->out_pending = 1;
	rc = usbd_io(l->out_urb, l->out, out_done, l, USBD_TIME_INFINITY);
	if (rc != EOK) {
		l->out_pending = 0;
		l->broken = 1;
		log_msg("usb: bulk OUT usbd_io: %s", strerror(rc));
		wake();
	}
}

static void link_send(struct link *l, uint8_t type, const void *payload, unsigned len)
{
	unsigned slot, n;

	pthread_mutex_lock(&l->lock);
	if (l->outq_n == OUT_QUEUE) {
		log_msg("usb: out queue full, frame type %u dropped", type);
	} else {
		slot = (l->outq_head + l->outq_n) % OUT_QUEUE;
		n = proto_encode(l->outq[slot], OUT_FRAME_MAX, type, payload, len);
		if (n > 0) {
			l->outq_len[slot] = n;
			l->outq_n++;
			out_kick(l);
		}
	}
	pthread_mutex_unlock(&l->lock);
}

static void out_done(struct usbd_urb *urb, struct usbd_pipe *pipe, void *arg)
{
	struct link *l = arg;
	uint32_t len;
	int ok = urb_ok(urb, &len);

	(void)pipe;
	pthread_mutex_lock(&l->lock);
	l->out_pending = 0;
	if (!l->closing) {
		if (ok) {
			l->outq_head = (l->outq_head + 1) % OUT_QUEUE;
			l->outq_n--;
			out_kick(l);
		} else {
			l->fail_status = urb_status(urb);
			log_msg("usb: bulk OUT failed, status 0x%08x", (unsigned)l->fail_status);
			l->broken = 1;
			wake();
		}
	}
	pthread_mutex_unlock(&l->lock);
}

/* Printable copy of a payload for the log: control chars as \n, \xNN. */
static void log_text(const char *what, unsigned seq, const char *s, unsigned n)
{
	static char buf[4 * PROTO_MAX_PAYLOAD + 1];
	unsigned i, o = 0;

	for (i = 0; i < n && o + 5 < sizeof(buf); i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '\n') {
			buf[o++] = '\\';
			buf[o++] = 'n';
		} else if (c < 0x20 || c == 0x7f) {
			o += (unsigned)snprintf(buf + o, sizeof(buf) - o, "\\x%02x", c);
		} else {
			buf[o++] = (char)c;
		}
	}
	buf[o] = 0;
	log_msg("%s seq=%u len=%u: %s", what, seq, n, buf);
}

static void send_caps(struct link *l)
{
	uint8_t caps[PROTO_VIDEO_CAPS_LEN];

	video_caps(caps);
	link_send(l, PROTO_VIDEO_CAPS, caps, sizeof(caps));
}

/* VIDEO_START / VIDEO_DATA; usbd thread. Bad pictures are acked at once,
 * good ones when they are on screen (video.c). */
static void on_video(struct link *l, uint8_t type, const uint8_t *p, unsigned n)
{
	struct vpic pic;
	int r, dropped = -1, bad = 0;

	if (type == PROTO_VIDEO_START) {
		r = vasm_start(&l->vasm, p, n, &dropped, &bad);
		if (dropped >= 0) {
			log_msg("video: picture %d cut short by the next one", dropped);
			video_ack((unsigned)dropped, PROTO_VIDEO_DROPPED, 0);
		}
	} else {
		r = vasm_data(&l->vasm, p, n, &bad);
	}
	switch (r) {
	case VASM_BAD:
		log_msg("rx %s for picture %d refused (%u bytes%s)",
		        type == PROTO_VIDEO_START ? "VIDEO_START" : "VIDEO_DATA", bad, n,
		        n >= PROTO_VIDEO_START_LEN && type == PROTO_VIDEO_START ?
		        ": bad size/format/dimensions" : "");
		video_ack((unsigned)bad, PROTO_VIDEO_BAD, 0);
		break;
	case VASM_DONE:
		vasm_take(&l->vasm, &pic);
		video_submit(&pic);
		break;
	default:
		break;
	}
}

static void set_peer(struct link *l, const uint8_t *p, unsigned n)
{
	char detail[MODEL_DETAIL_LEN];

	if (n > sizeof(detail) - 1)
		n = sizeof(detail) - 1;
	memcpy(detail, p, n);
	detail[n] = 0;
	l->hello_seen = 1;
	model_set_state(LINK_CONNECTED, detail);
}

/* Runs on the usbd thread (from in_done). */
static void on_frame(uint8_t type, const uint8_t *p, unsigned n, void *arg)
{
	struct link *l = arg;
	uint8_t ack[2];

	l->rx_frames++;
	switch (type) {
	case PROTO_HELLO:
		log_text("rx HELLO", 0, (const char *)p, n);
		link_send(l, PROTO_HELLO_REPLY, USBTX_HELLO, sizeof(USBTX_HELLO) - 1);
		send_caps(l);
		set_peer(l, p, n);
		break;
	case PROTO_HELLO_REPLY:
		log_text("rx HELLO_REPLY", 0, (const char *)p, n);
		set_peer(l, p, n);
		break;
	case PROTO_TEXT:
		if (n < 2) {
			log_msg("rx TEXT too short (%u bytes), ignored", n);
			break;
		}
		log_text("rx TEXT", proto_get_u16(p), (const char *)p + 2, n - 2);
		model_add_message(proto_get_u16(p), (const char *)p + 2, n - 2);
		ack[0] = p[0];
		ack[1] = p[1];
		link_send(l, PROTO_ACK, ack, 2);
		if (!l->hello_seen)
			set_peer(l, (const uint8_t *)"phone app", 9);
		break;
	case PROTO_VIDEO_START:
	case PROTO_VIDEO_DATA:
		on_video(l, type, p, n);
		if (!l->hello_seen)
			set_peer(l, (const uint8_t *)"phone app", 9);
		break;
	case PROTO_VIDEO_STOP:
		log_msg("rx VIDEO_STOP");
		vasm_free(&l->vasm);
		video_stop("the phone stopped streaming");
		break;
	default:
		log_msg("rx unknown frame type 0x%02x, %u bytes, ignored", type, n);
		break;
	}
}

/* l->lock held. */
static int in_post(struct link *l)
{
	int rc;

	usbd_setup_bulk(l->in_urb, URB_DIR_IN | URB_SHORT_XFER_OK, l->in_buf, IN_BUF_SIZE);
	l->in_pending = 1;
	rc = usbd_io(l->in_urb, l->in, in_done, l, USBD_TIME_INFINITY);
	if (rc != EOK) {
		l->in_pending = 0;
		log_msg("usb: bulk IN usbd_io: %s", strerror(rc));
	}
	return rc;
}

static void in_done(struct usbd_urb *urb, struct usbd_pipe *pipe, void *arg)
{
	struct link *l = arg;
	uint32_t len;
	int ok = urb_ok(urb, &len);

	(void)pipe;
	pthread_mutex_lock(&l->lock);
	l->in_pending = 0;
	if (l->closing) {
		pthread_mutex_unlock(&l->lock);
		return;
	}
	if (!ok) {
		l->fail_status = urb_status(urb);
		log_msg("usb: bulk IN failed, status 0x%08x", (unsigned)l->fail_status);
		l->broken = 1;
		pthread_mutex_unlock(&l->lock);
		wake();
		return;
	}
	pthread_mutex_unlock(&l->lock);

	if (len > IN_BUF_SIZE)
		len = IN_BUF_SIZE;
	proto_dec_feed(&l->dec, l->in_buf, len, on_frame, l);   /* may link_send() */

	pthread_mutex_lock(&l->lock);
	if (!l->closing && in_post(l) != EOK) {
		l->broken = 1;
		wake();
	}
	pthread_mutex_unlock(&l->lock);
}

/* libusbdi counts a URB as pending until its completion callback has
 * returned (usbdi_event_thread decrements pipe+0xc after calling it), and
 * usbd_close_pipe() / usbd_detach() refuse with EBUSY - freeing nothing -
 * while anything is pending. Retry until the counters drain. */
static int close_pipe_wait(struct usbd_pipe *pipe)
{
	unsigned t0 = log_ms();
	int rc;

	while ((rc = usbd_close_pipe(pipe)) == EBUSY && log_ms() - t0 < TEARDOWN_WAIT_MS)
		usleep(10 * 1000);
	if (rc != EOK)
		log_msg("usb:   usbd_close_pipe: %s", strerror(rc));
	return rc;
}

/* Frees the link, unless a pipe will not close: then a URB may still be in
 * flight into our buffers, so everything is leaked instead. */
static void link_free(struct link *l)
{
	if ((l->in != NULL && close_pipe_wait(l->in) != EOK) ||
	    (l->out != NULL && close_pipe_wait(l->out) != EOK)) {
		log_msg("usb: pipe still busy, leaking the link");
		return;
	}
	if (l->in_urb != NULL)
		usbd_free_urb(l->in_urb);
	if (l->out_urb != NULL)
		usbd_free_urb(l->out_urb);
	if (l->in_buf != NULL)
		usbd_free(l->in_buf);
	if (l->out_buf != NULL)
		usbd_free(l->out_buf);
	vasm_free(&l->vasm);
	pthread_mutex_destroy(&l->lock);
	free(l);
}

/* Abort both pipes and wait for the completions, then free. If io-usb
 * never completes them the link is leaked rather than freed under it. */
static void link_close(struct link *l)
{
	unsigned t0 = log_ms();
	int pending;

	pthread_mutex_lock(&l->lock);
	l->closing = 1;
	pthread_mutex_unlock(&l->lock);
	if (l->in != NULL)
		usbd_abort_pipe(l->in);
	if (l->out != NULL)
		usbd_abort_pipe(l->out);
	for (;;) {
		pthread_mutex_lock(&l->lock);
		pending = l->in_pending || l->out_pending;
		pthread_mutex_unlock(&l->lock);
		if (!pending || log_ms() - t0 > TEARDOWN_WAIT_MS)
			break;
		usleep(10 * 1000);
	}
	if (pending) {
		log_msg("usb: transfers still pending after %u ms, leaking the link",
		        TEARDOWN_WAIT_MS);
		return;
	}
	link_free(l);
	log_msg("usb: link closed (%u ms)", log_ms() - t0);
}

/* Find the accessory interface's bulk endpoints and open the link. */
static struct link *link_open(struct usbd_device *dev)
{
	usbd_descriptors_t *cfg, *ifc, *ep, *ep_in = NULL, *ep_out = NULL;
	struct usbd_desc_node *dnode = NULL, *cnode, *inode;
	pthread_mutexattr_t ma;
	struct link *l;
	int c, i, e, rc;

	if (usbd_device_descriptor(dev, &dnode) == NULL) {
		log_msg("usb: accessory: no device descriptor");
		return NULL;
	}
	for (c = 0; ep_in == NULL || ep_out == NULL; c++) {
		cfg = usbd_parse_descriptors(dev, dnode, USB_DESC_CONFIGURATION, c, &cnode);
		if (cfg == NULL)
			break;
		for (i = 0; ep_in == NULL || ep_out == NULL; i++) {
			ifc = usbd_parse_descriptors(dev, cnode, USB_DESC_INTERFACE, i, &inode);
			if (ifc == NULL)
				break;
			log_msg("usb:   config %u iface %u: class %02x/%02x/%02x, %u endpoints",
			        cfg->configuration.bConfigurationValue,
			        ifc->interface.bInterfaceNumber, ifc->interface.bInterfaceClass,
			        ifc->interface.bInterfaceSubClass, ifc->interface.bInterfaceProtocol,
			        ifc->interface.bNumEndpoints);
			/* accessory = ff/ff; the adb interface of 2d01 is ff/42 */
			if (ifc->interface.bInterfaceClass != 0xff ||
			    ifc->interface.bInterfaceSubClass != 0xff)
				continue;
			ep_in = ep_out = NULL;
			for (e = 0; (ep = usbd_parse_descriptors(dev, inode, USB_DESC_ENDPOINT,
			                                         e, NULL)) != NULL; e++) {
				if ((ep->endpoint.bmAttributes & 3) != USB_ATTRIB_BULK)
					continue;
				if (ep->endpoint.bEndpointAddress & USB_ENDPOINT_IN) {
					if (ep_in == NULL)
						ep_in = ep;
				} else if (ep_out == NULL) {
					ep_out = ep;
				}
			}
		}
	}
	if (ep_in == NULL || ep_out == NULL) {
		log_msg("usb: accessory: no ff/ff interface with bulk IN + OUT");
		return NULL;
	}
	log_msg("usb: accessory endpoints IN 0x%02x (%u B) OUT 0x%02x (%u B)",
	        ep_in->endpoint.bEndpointAddress, ep_in->endpoint.wMaxPacketSize,
	        ep_out->endpoint.bEndpointAddress, ep_out->endpoint.wMaxPacketSize);

	l = calloc(1, sizeof(*l));
	if (l == NULL)
		return NULL;
	/* Recursive: usbd_io() is called with the lock held, and should io-usb
	 * ever run the completion callback inline it takes the lock again. */
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&l->lock, &ma);
	pthread_mutexattr_destroy(&ma);
	l->dev = dev;
	proto_dec_init(&l->dec);
	vasm_init(&l->vasm, VIDEO_MAX_BYTES);
	rc = usbd_open_pipe(dev, ep_in, &l->in);
	if (rc == EOK)
		rc = usbd_open_pipe(dev, ep_out, &l->out);
	if (rc != EOK) {
		log_msg("usb: usbd_open_pipe: %s", strerror(rc));
		link_free(l);
		return NULL;
	}
	l->in_urb = usbd_alloc_urb(NULL);
	l->out_urb = usbd_alloc_urb(NULL);
	l->in_buf = usbd_alloc(IN_BUF_SIZE);
	l->out_buf = usbd_alloc(OUT_FRAME_MAX);
	if (!l->in_urb || !l->out_urb || !l->in_buf || !l->out_buf) {
		log_msg("usb: out of USB memory");
		link_free(l);
		return NULL;
	}
	pthread_mutex_lock(&l->lock);
	rc = in_post(l);
	pthread_mutex_unlock(&l->lock);
	if (rc != EOK) {
		link_free(l);
		return NULL;
	}
	link_send(l, PROTO_HELLO, USBTX_HELLO, sizeof(USBTX_HELLO) - 1);
	send_caps(l);
	return l;
}

/* --- AOA handshake ----------------------------------------------------- */

static const char *status_name(uint32_t st)
{
	if (st & USBD_STATUS_TIMEOUT)
		return "timeout";
	if (st & USBD_STATUS_ABORTED)
		return "aborted";
	if (st & USBD_STATUS_DEVICE_RESET)
		return "device reset";
	switch (st & ~USBD_URB_STATUS_MASK) {
	case USBD_STATUS_STALL:         return "stall";
	case USBD_STATUS_DEV_NOANSWER:  return "device did not answer";
	case USBD_STATUS_DATA_UNDERRUN: return "data underrun";
	}
	return "?";
}

static int ctrl(struct usbd_pipe *pipe, struct usbd_urb *urb, int in, uint8_t req,
                uint16_t index, uint8_t *buf, uint32_t len, uint32_t *got)
{
	uint16_t rtype = USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE |
	                 (in ? USB_DIRECTION_HOST : USB_DIRECTION_DEVICE);
	int rc;

	unsigned t0 = log_ms();

	usbd_setup_vendor(urb, in ? URB_DIR_IN : URB_DIR_OUT, req, rtype, 0, index, buf, len);
	rc = usbd_io(urb, pipe, NULL, NULL, CTRL_TIMEOUT_MS);
	if (rc == EOK && !urb_ok(urb, got)) {
		uint32_t st = urb_status(urb);

		log_msg("usb:   request %u index %u: status 0x%08x (%s) after %u ms", req, index,
		        (unsigned)st, status_name(st), log_ms() - t0);
		rc = EIO;
	}
	return rc;
}

/* 1 = START sent (phone will re-enumerate), 0 = not an AOA device (it
 * refused GET_PROTOCOL), -1 = a request failed some other way, or the
 * device speaks AOA but a later request failed: worth another try. */
static int aoa_switch(struct usbd_device *dev)
{
	static const char *const strings[6] = {
		AOA_MANUFACTURER, AOA_MODEL, AOA_DESCRIPTION,
		AOA_VERSION, AOA_URI, AOA_SERIAL,
	};
	usbd_descriptors_t *dd;
	struct usbd_pipe *pipe = NULL;
	struct usbd_urb *urb = NULL;
	uint8_t *buf = NULL;
	uint32_t got = 0;
	int rc, i, ok = 0, version;

	dd = usbd_parse_descriptors(dev, NULL, USB_DESC_DEVICE, 0, NULL);
	if (dd == NULL || (rc = usbd_open_pipe(dev, dd, &pipe)) != EOK) {
		log_msg("usb:   no control pipe");
		return -1;
	}
	urb = usbd_alloc_urb(NULL);
	buf = usbd_alloc(256);
	if (urb == NULL || buf == NULL) {
		ok = -1;
		goto out;
	}

	buf[0] = buf[1] = 0;
	rc = ctrl(pipe, urb, 1, AOA_GET_PROTOCOL, 0, buf, 2, &got);
	version = buf[0] | (buf[1] << 8);
	log_msg("usb:   AOA GET_PROTOCOL: %s, %u bytes, version %d",
	        rc ? strerror(rc) : "ok", (unsigned)got, version);
	if (rc == EIO && (urb_status(urb) & ~USBD_URB_STATUS_MASK) != USBD_STATUS_STALL)
		ok = -1;                      /* no answer at all, not a refusal: try again */
	if (rc != EOK || got < 2 || version < 1)
		goto out;
	ok = -1;                          /* an AOA device from here on */

	for (i = 0; i < 6; i++) {
		size_t n = strlen(strings[i]) + 1;

		memcpy(buf, strings[i], n);
		rc = ctrl(pipe, urb, 0, AOA_SEND_STRING, (uint16_t)i, buf, (uint32_t)n, &got);
		if (rc != EOK) {
			log_msg("usb:   AOA SEND_STRING %d: %s", i, strerror(rc));
			goto out;
		}
	}
	rc = ctrl(pipe, urb, 0, AOA_START, 0, buf, 0, &got);
	log_msg("usb:   AOA START: %s", rc ? strerror(rc) : "ok");
	if (rc == EOK)
		ok = 1;
out:
	if (buf != NULL)
		usbd_free(buf);
	if (urb != NULL)
		usbd_free_urb(urb);
	close_pipe_wait(pipe);
	return ok;
}

/* --- device bookkeeping (manager thread only) -------------------------- */

struct dev_entry {
	int                    used;
	uint8_t                path, devno;
	uint16_t               generation;
	struct usbd_device    *dev;       /* still attached, or NULL */
	usbd_device_instance_t inst;      /* the instance we attached through */
	int                    is_acc;    /* an AOA accessory (18d1:2d0x) */
	unsigned               reopen_at; /* log_ms() to reopen the accessory, 0 = no */
	int                    reopen_tries;
	unsigned               inserted_at;  /* log_ms() of the insertion */
	unsigned               handshake_at; /* log_ms() of the next AOA handshake, 0 = no */
	int                    handshake_tries;
	int                    retryable; /* a handshake failed in a way worth retrying */
};

static struct usbd_connection *conn;
static struct dev_entry devs[MAX_DEVICES];
static struct link *acc;              /* the accessory link, or NULL */
static struct dev_entry *acc_entry;
static unsigned switch_since;         /* log_ms() of AOA START, 0 = none */

static struct dev_entry *dev_find(const usbd_device_instance_t *in)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++)
		if (devs[i].used && devs[i].path == in->path && devs[i].devno == in->devno &&
		    devs[i].generation == in->generation)
			return &devs[i];
	return NULL;
}

static void dev_release(struct dev_entry *d)
{
	/* A device we fail to detach stays "open" in io-usb, which then holds
	 * its port off even after an unplug (CLASS_ExtractDevice), so retry
	 * while libusbdi still counts I/O as pending. */
	if (d->dev != NULL) {
		unsigned t0 = log_ms();
		int rc;

		while ((rc = usbd_detach(d->dev)) == EBUSY && log_ms() - t0 < TEARDOWN_WAIT_MS)
			usleep(10 * 1000);
		if (rc != EOK)
			log_msg("usb:   usbd_detach: %s", strerror(rc));
		d->dev = NULL;
	}
}

static void link_drop(const char *why, enum link_state next)
{
	if (acc == NULL)
		return;
	log_msg("usb: accessory link down: %s", why);
	link_close(acc);
	acc = NULL;
	video_stop("USB link down");
	if (acc_entry != NULL)
		dev_release(acc_entry);
	acc_entry = NULL;
	model_set_state(next, why);
}

/* Open the accessory link on an attached accessory entry. */
static int acc_open(struct dev_entry *d, const char *detail)
{
	/* Before link_open(): its reads may set CONNECTED right away. */
	model_set_state(LINK_ACCESSORY, detail);
	acc = link_open(d->dev);
	if (acc == NULL)
		return -1;
	acc_entry = d;
	return 0;
}

/* The link broke but the device was not removed (yet). Seen when another
 * client of the same device detaches - io-usb's udi_detach aborts every URB
 * queued on the interface's endpoints, ours included - or when the media
 * launcher's MTP probe resets the device. An unplug also aborts our URBs
 * first; its removal event follows and cancels the reopen. */
static void acc_broken(void)
{
	struct dev_entry *d = acc_entry;
	uint32_t status;
	int rx;

	pthread_mutex_lock(&acc->lock);
	status = acc->fail_status;
	rx = acc->rx_frames;
	pthread_mutex_unlock(&acc->lock);
	log_msg("usb: accessory link broken (status 0x%08x, %d frames received)",
	        (unsigned)status, rx);
	link_drop("USB link interrupted - reconnecting", LINK_ACCESSORY);
	if (d != NULL) {
		if (rx > 0)
			d->reopen_tries = 0;      /* it worked for a while: fresh retries */
		d->reopen_at = log_ms() + REOPEN_DELAY_MS;
		if (d->reopen_at == 0)
			d->reopen_at = 1;
	}
}

static void acc_reopen_due(void)
{
	struct dev_entry *d;
	unsigned now = log_ms();
	int i, rc;

	for (i = 0; i < MAX_DEVICES && acc == NULL; i++) {
		d = &devs[i];
		if (!d->used || d->reopen_at == 0 || (int)(now - d->reopen_at) < 0)
			continue;
		d->reopen_at = 0;
		if (++d->reopen_tries > REOPEN_MAX) {
			log_msg("usb: accessory reopen: giving up after %d tries", REOPEN_MAX);
			model_set_state(LINK_WAITING, "USB link keeps failing - replug the phone");
			continue;
		}
		rc = usbd_attach(conn, &d->inst, 0, &d->dev);
		log_msg("usb: accessory reopen %d/%d: attach %s", d->reopen_tries, REOPEN_MAX,
		        rc ? strerror(rc) : "ok");
		if (rc != EOK) {
			d->dev = NULL;            /* gone: its removal event will follow */
			continue;
		}
		if (acc_open(d, "USB link interrupted - reconnecting") != 0) {
			dev_release(d);
			d->reopen_at = now + REOPEN_DELAY_MS * (unsigned)d->reopen_tries;
			if (d->reopen_at == 0)
				d->reopen_at = 1;
		}
	}
}

static unsigned due_in(unsigned ms)
{
	unsigned t = log_ms() + ms;

	return t ? t : 1;
}

/* Attempt the AOA handshake on a (non-accessory) device whose time has come.
 * The 12.10 run: the phone answered GET_PROTOCOL (v2) and two strings, then
 * SEND_STRING 2 came back "device did not answer" and the phone was given up
 * on for good. So a device that has shown it speaks AOA gets more tries. */
static void handshake_due(void)
{
	struct dev_entry *d;
	unsigned now = log_ms();
	int i, rc, r;

	for (i = 0; i < MAX_DEVICES; i++) {
		d = &devs[i];
		if (!d->used || d->handshake_at == 0 || (int)(now - d->handshake_at) < 0)
			continue;
		d->handshake_at = 0;
		d->handshake_tries++;
		rc = usbd_attach(conn, &d->inst, 0, &d->dev);
		if (rc != EOK) {
			log_msg("usb: AOA handshake %d/%d: attach %s", d->handshake_tries,
			        HANDSHAKE_TRIES, strerror(rc));
			d->dev = NULL;            /* gone: its removal event will follow */
			continue;
		}
		log_msg("usb: AOA handshake %d/%d on %04x:%04x, %u ms after insertion",
		        d->handshake_tries, HANDSHAKE_TRIES, (unsigned)d->inst.ident.vendor,
		        (unsigned)d->inst.ident.device, now - d->inserted_at);
		r = aoa_switch(d->dev);
		dev_release(d);               /* keep the entry until it is removed */
		if (r < 0)
			d->retryable = 1;
		if (r > 0) {
			switch_since = due_in(0);
			model_set_state(LINK_SWITCHING, "phone is switching to accessory mode");
			continue;
		}
		if (r == 0 && !d->retryable) {
			log_msg("usb:   not an Android (AOA) device, ignored");
			continue;
		}
		if (d->handshake_tries >= HANDSHAKE_TRIES) {
			log_msg("usb:   AOA handshake failed %d times, giving up", HANDSHAKE_TRIES);
			model_set_state(LINK_WAITING, "accessory handshake failed - replug the phone");
			continue;
		}
		d->handshake_at = due_in(HANDSHAKE_DELAY_MS * (unsigned)d->handshake_tries);
		log_msg("usb:   AOA handshake failed, retrying in %u ms",
		        HANDSHAKE_DELAY_MS * (unsigned)d->handshake_tries);
		if (d->handshake_tries == 1)
			slog_snapshot("first handshake failure");
	}
}

static void on_insert(usbd_device_instance_t *in)
{
	struct dev_entry *d = dev_find(in);
	usbd_device_descriptor_t *dd;
	struct usbd_desc_node *node = NULL;
	char detail[80];
	int i, rc;

	if (d != NULL)                    /* another interface of a known device */
		return;
	/* An accessory arrives once per interface. Attach through the accessory
	 * interface (ff/ff), not adb (ff/42) or audio: sdl_core's QNX transport
	 * filters its AOA connection the same way. */
	if (in->ident.vendor == AOA_VID && in->ident.device >= AOA_PID_MIN &&
	    in->ident.device <= AOA_PID_MAX && in->ident.dclass == 0xff &&
	    in->ident.subclass != 0xff) {
		log_msg("usb: insert %04x:%04x iface %u class ff/%02x: not the accessory "
		        "interface, skipped", (unsigned)in->ident.vendor, (unsigned)in->ident.device,
		        (unsigned)in->iface, (unsigned)in->ident.subclass);
		return;
	}
	for (i = 0; i < MAX_DEVICES && devs[i].used; i++)
		;
	if (i == MAX_DEVICES) {
		log_msg("usb: too many devices, insert ignored");
		return;
	}
	d = &devs[i];
	memset(d, 0, sizeof(*d));
	d->used = 1;
	d->path = in->path;
	d->devno = in->devno;
	d->generation = in->generation;
	d->inst = *in;
	d->inserted_at = log_ms();

	rc = usbd_attach(conn, in, 0, &d->dev);
	log_msg("usb: insert path %u devno %u gen %u iface %u, %04x:%04x class %02x/%02x/%02x: "
	        "attach %s", in->path, in->devno, in->generation, (unsigned)in->iface,
	        (unsigned)in->ident.vendor, (unsigned)in->ident.device,
	        (unsigned)in->ident.dclass, (unsigned)in->ident.subclass,
	        (unsigned)in->ident.protocol, rc ? strerror(rc) : "ok");
	if (rc != EOK) {
		d->dev = NULL;
		return;
	}
	dd = usbd_device_descriptor(d->dev, &node);
	if (dd == NULL) {
		log_msg("usb:   no device descriptor, ignored");
		dev_release(d);
		return;
	}
	log_msg("usb:   device %04x:%04x usb %x.%02x class %02x, %u configs",
	        dd->idVendor, dd->idProduct, dd->bcdUSB >> 8, dd->bcdUSB & 0xff,
	        dd->bDeviceClass, dd->bNumConfigurations);

	if (dd->bDeviceClass == USB_CLASS_HUB) {
		log_msg("usb:   hub, ignored");
		dev_release(d);
		return;
	}
	if (dd->idVendor == AOA_VID && dd->idProduct >= AOA_PID_MIN &&
	    dd->idProduct <= AOA_PID_MAX) {
		if (acc != NULL) {
			log_msg("usb:   a second accessory, ignored");
			dev_release(d);
			return;
		}
		switch_since = 0;
		d->is_acc = 1;
		if (acc_open(d, "open usb_tx_test on the phone") != 0) {
			dev_release(d);
			model_set_state(LINK_WAITING, "accessory open failed - replug the phone");
		}
		return;
	}
	/* Stay off EP0 until the media launcher is done with the device. Its
	 * insertion callback (vdev-medialaunch vml_insertion, P1001) runs at
	 * the same moment as ours: an MTP probe, GET_DESCRIPTOR(0xEE) on EP0,
	 * that calls usbd_reset_device on any failure but a STALL
	 * (0x080539c0), then usbd_select_config (SET_CONFIGURATION), then
	 * the bus event thread waits its -c delay (default 2000 ms, srv-starter
	 * passes none) before it starts the MTP driver. On 12.10 our handshake
	 * ran inside that window (+0.2 s in v1, +1.3 s in v2): GET_PROTOCOL
	 * answered, then DEV_NOANSWER on the next request and on everything
	 * after it - a reset device - and io-usb reported the phone removed
	 * 3-5 s later. The handshake now waits HANDSHAKE_FIRST_MS. If the
	 * phone is reset anyway, its re-insertion starts over with a new entry. */
	snprintf(detail, sizeof(detail), "USB device %04x:%04x found - handshake in %u s",
	         (unsigned)dd->idVendor, (unsigned)dd->idProduct, HANDSHAKE_FIRST_MS / 1000);
	dev_release(d);                   /* keep the entry until it is removed; frees dd */
	d->handshake_at = due_in(HANDSHAKE_FIRST_MS);
	model_set_state(LINK_WAITING, detail);
}

/* The system log at this moment, into <sd>/var/usb_slog_<n>.txt, in the
 * background (sloginfo + SD write take about a second). The run ends by
 * pulling the card, so run.sh never gets to save usb_tx_test's slog; these
 * snapshots are what shows io-usb's and the media launcher's side of a
 * failed handshake (12.10: DEV_NOANSWER on SEND_STRING, cause unknown). */
static const char *slog_sd;

static void slog_snapshot(const char *why)
{
	static int n;
	char cmd[400];

	if (slog_sd == NULL || n >= AOA_SLOG_SNAPSHOTS)
		return;
	n++;
	snprintf(cmd, sizeof(cmd), "sloginfo > '%s/var/usb_slog_%d.txt' 2>&1 &", slog_sd, n);
	log_msg("usb: slog snapshot %d (%s) -> var/usb_slog_%d.txt", n, why, n);
	if (system(cmd) != 0)
		log_msg("usb:   could not start sloginfo");
}

static void on_remove(usbd_device_instance_t *in)
{
	struct dev_entry *d = dev_find(in);

	if (d == NULL)                    /* other interfaces of a gone device */
		return;
	log_msg("usb: remove path %u devno %u gen %u", in->path, in->devno, in->generation);
	slog_snapshot("device removed");
	if (d == acc_entry)
		link_drop("phone disconnected", LINK_WAITING);
	else if (d->is_acc)               /* broken earlier, reopen pending */
		model_set_state(LINK_WAITING, "phone disconnected");
	dev_release(d);
	d->used = 0;
}

/* --- manager thread ---------------------------------------------------- */

static int try_connect(unsigned attempt)
{
	static usbd_device_ident_t ident = {
		USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD,
		USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD,
	};
	static usbd_funcs_t funcs = { _USBDI_NFUNCS, cb_insertion, cb_removal, NULL };
	usbd_connect_parm_t parm;
	char detail[MODEL_DETAIL_LEN];
	int rc;

	memset(&parm, 0, sizeof(parm));
	parm.vusb = USB_VERSION;
	parm.vusbd = USBD_VERSION;
	parm.ident = &ident;
	parm.funcs = &funcs;
	rc = usbd_connect(&parm, &conn);
	if (rc == EOK) {
		log_msg("usb: usbd_connect ok (attempt %u)", attempt);
		model_set_state(LINK_WAITING, "plug the phone into the car's USB port");
		return 0;
	}
	conn = NULL;
	if (attempt == 1 || attempt % 30 == 0)
		log_msg("usb: usbd_connect: %s (attempt %u)", strerror(rc), attempt);
	snprintf(detail, sizeof(detail), "usbd_connect: %s - retrying", strerror(rc));
	model_set_state(LINK_NO_STACK, detail);
	return -1;
}

/* Wait for an event (or a kick / stop) up to ms; qlock held. */
static void wait_locked(unsigned ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	while (qlen == 0 && !kick && !stopping)
		if (pthread_cond_timedwait(&qcond, &qlock, &ts) == ETIMEDOUT)
			break;
	kick = 0;
}

static void *manager(void *arg)
{
	unsigned attempt = 0;
	uint8_t ack[PROTO_VIDEO_ACK_LEN];
	struct event ev;
	int i, have, broken;

	(void)arg;
	for (;;) {
		pthread_mutex_lock(&qlock);
		if (stopping) {
			pthread_mutex_unlock(&qlock);
			break;
		}
		if (conn == NULL) {
			pthread_mutex_unlock(&qlock);
			if (try_connect(++attempt) != 0) {
				pthread_mutex_lock(&qlock);
				wait_locked(CONNECT_RETRY_MS);
				pthread_mutex_unlock(&qlock);
			}
			continue;
		}
		wait_locked(500);
		have = qlen > 0;
		if (have) {
			ev = queue[qhead];
			qhead = (qhead + 1) % MAX_EVENTS;
			qlen--;
		}
		pthread_mutex_unlock(&qlock);

		if (have) {
			if (ev.kind == EV_INSERT)
				on_insert(&ev.inst);
			else
				on_remove(&ev.inst);
		}
		if (acc != NULL) {
			pthread_mutex_lock(&acc->lock);
			broken = acc->broken;
			pthread_mutex_unlock(&acc->lock);
			if (broken)
				acc_broken();
		}
		while (video_ack_pop(ack))         /* no link: they go nowhere */
			if (acc != NULL)
				link_send(acc, PROTO_VIDEO_ACK, ack, sizeof(ack));
		if (acc == NULL)
			acc_reopen_due();
		handshake_due();
		if (switch_since != 0 && log_ms() - switch_since > SWITCH_TIMEOUT_MS) {
			switch_since = 0;
			model_set_state(LINK_WAITING, "phone did not come back as an accessory - replug it");
		}
	}

	log_msg("usb: manager stopping");
	link_drop("stopping", LINK_WAITING);
	for (i = 0; i < MAX_DEVICES; i++)
		if (devs[i].used)
			dev_release(&devs[i]);
	if (conn != NULL) {
		int rc = usbd_disconnect(conn);

		log_msg("usb: usbd_disconnect: %s", rc ? strerror(rc) : "ok");
		conn = NULL;
	}
	return NULL;
}

int aoa_start(const char *sd_path)
{
	slog_sd = sd_path;
	pthread_condattr_t ca;
	int rc;

	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&qcond, &ca);
	pthread_condattr_destroy(&ca);
	video_set_notify(wake);
	rc = pthread_create(&mgr_tid, NULL, manager, NULL);
	if (rc != 0) {
		log_msg("usb: manager thread: %s", strerror(rc));
		return -1;
	}
	mgr_started = 1;
	return 0;
}

void aoa_stop(void)
{
	if (!mgr_started)
		return;
	pthread_mutex_lock(&qlock);
	stopping = 1;
	pthread_cond_signal(&qcond);
	pthread_mutex_unlock(&qlock);
	pthread_join(mgr_tid, NULL);
	mgr_started = 0;
}

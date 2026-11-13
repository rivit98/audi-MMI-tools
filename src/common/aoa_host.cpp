// aoa_host.cpp - see aoa_host.h. Derived from audi_stream_receiver's aoa.cpp;
// the comments that explain io-usb's and the media launcher's behaviour are
// kept with the code they explain.
//
// USBDI usage follows smartdevicelink's QNX transport (usb_handler.cc /
// usb_connection.cc), which shipped on QNX head units: synchronous vendor
// control transfers on the default pipe for the AOA handshake, asynchronous
// bulk transfers for data, a short transfer reported as
// USBD_STATUS_CMP_ERR | USBD_STATUS_DATA_UNDERRUN counting as success.
#include "aoa_host.h"
#include "types.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#define _Static_assert static_assert   // the SDK's usbdi.h checks its layouts the C way
#include <sys/usbdi.h>
#undef _Static_assert

extern "C" {
#include "cpumon.h"
#include "log.h"
}

#include "mono_cond.h"
#include "usb_chunk.h"

namespace aoa_host {

namespace {

// AOA protocol (source.android.com/docs/core/interaction/accessories/aoa)
constexpr u8  kAoaGetProtocol = 51;
constexpr u8  kAoaSendString = 52;
constexpr u8  kAoaStart = 53;
constexpr unsigned kAoaVid = 0x18D1;
constexpr unsigned kAoaPidMin = 0x2D00;   // 2d00 accessory, 2d01 +adb, 2d04/05 +audio
constexpr unsigned kAoaPidMax = 0x2D05;

constexpr unsigned kCtrlTimeoutMs = 2000;
constexpr unsigned kConnectRetryMs = 2000;
constexpr unsigned kSwitchTimeoutMs = 10000;
constexpr unsigned kTeardownWaitMs = 3000;
constexpr unsigned kReopenDelayMs = 500;
constexpr int      kReopenMax = 5;
constexpr unsigned kHandshakeFirstMs = 4000;  // after insertion: media launcher done (below)
constexpr unsigned kHandshakeDelayMs = 1000;  // retry n waits n times this
constexpr int      kHandshakeTries = 5;
constexpr unsigned kInBufSize = 16384;        // Android accessory reads want >= 16 KB
constexpr unsigned kOutBufSize = 16384;
constexpr int      kMaxDevices = 16;
constexpr usize   kMaxEvents = 32;

Config  cfg;
Client *client;

// --- events from io-usb callbacks -> manager thread ----------------------

enum class EvKind { Insert, Remove };

struct Event {
	EvKind                 kind;
	usbd_device_instance_t inst;
};

std::mutex        qlock;
MonoCond          qcond;
std::deque<Event> queue;
bool              stopping;      // stop() called
bool              kick;          // manager has work besides events
std::thread       manager_thread;
bool              manager_started;

void slog_snapshot(const char *why);

void wake()
{
	{
		std::lock_guard<std::mutex> lock(qlock);
		kick = true;
	}
	qcond.notify();
}

void post(EvKind kind, const usbd_device_instance_t *inst)
{
	{
		std::lock_guard<std::mutex> lock(qlock);
		if (queue.size() < kMaxEvents)
			queue.push_back(Event{kind, *inst});
		else
			log_msg("usb: event queue full, %s dropped",
			        kind == EvKind::Insert ? "insert" : "remove");
	}
	qcond.notify();
}

// io-usb's callback thread(s), named for cpumon; a compare per call.
void name_usbd_thread()
{
	static pthread_t named;

	if (pthread_self() != named) {
		named = pthread_self();
		cpumon_name_thread("usbd");
	}
}

void cb_insertion(struct usbd_connection *, usbd_device_instance_t *inst)
{
	name_usbd_thread();
	post(EvKind::Insert, inst);
}

void cb_removal(struct usbd_connection *, usbd_device_instance_t *inst)
{
	post(EvKind::Remove, inst);
}

State cur_state = State::NoStack;

void set_state(State s, const char *detail)
{
	cur_state = s;
	log_msg("usb: state %s: %s", state_name(s), detail);
	client->state(s, detail);
}

// --- what io-usb hands out -----------------------------------------------

struct UrbFree {
	void operator()(struct usbd_urb *u) const { usbd_free_urb(u); }
};
using Urb = std::unique_ptr<struct usbd_urb, UrbFree>;

struct UsbFree {
	void operator()(u8 *p) const { usbd_free(p); }
};
using UsbBuf = std::unique_ptr<u8, UsbFree>;

UsbBuf usb_alloc(usize n)
{
	return UsbBuf(static_cast<u8 *>(usbd_alloc(n)));
}

bool urb_ok(struct usbd_urb *urb, u32 *len)
{
	u32 status = 0;

	*len = 0;
	int rc = usbd_urb_status(urb, &status, len);
	if (rc != EOK && rc != EIO)
		return false;
	return (status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP ||
	       status == (USBD_STATUS_CMP_ERR | USBD_STATUS_DATA_UNDERRUN);
}

u32 urb_status(struct usbd_urb *urb)
{
	u32 status = 0, len = 0;

	usbd_urb_status(urb, &status, &len);
	return status;
}

// libusbdi counts a URB as pending until its completion callback has
// returned (usbdi_event_thread decrements pipe+0xc after calling it), and
// usbd_close_pipe() / usbd_detach() refuse with EBUSY - freeing nothing -
// while anything is pending. Retry until the counters drain.
int close_pipe_wait(struct usbd_pipe *pipe)
{
	unsigned t0 = log_ms();
	int rc;

	while ((rc = usbd_close_pipe(pipe)) == EBUSY && log_ms() - t0 < kTeardownWaitMs)
		usleep(10 * 1000);
	if (rc != EOK)
		log_msg("usb:   usbd_close_pipe: %s", strerror(rc));
	return rc;
}

// --- the accessory link ----------------------------------------------------

class Link {
public:
	// Finds the accessory interface's bulk endpoints and opens the link;
	// nullptr if that fails.
	static std::unique_ptr<Link> open(struct usbd_device *dev);

	bool send(const u8 *p, usize n);
	usize pending()
	{
		std::lock_guard<std::recursive_mutex> lock(lock_);
		return outq_bytes_;
	}
	bool broken()
	{
		std::lock_guard<std::recursive_mutex> lock(lock_);
		return broken_;
	}
	u32 fail_status()
	{
		std::lock_guard<std::recursive_mutex> lock(lock_);
		return fail_status_;
	}
	u64 rx_bytes() const { return rx_bytes_; }

	// Frees the link, unless a pipe will not close: then a URB may still be
	// in flight into our buffers, so everything is leaked instead.
	static void dispose(std::unique_ptr<Link> l);
	// Aborts both pipes and waits for the completions, then frees. If io-usb
	// never completes them the link is leaked rather than freed under it.
	static void close(std::unique_ptr<Link> l);

private:
	Link() = default;

	static void out_done_cb(struct usbd_urb *urb, struct usbd_pipe *, void *arg)
	{
		static_cast<Link *>(arg)->out_done(urb);
	}
	static void in_done_cb(struct usbd_urb *urb, struct usbd_pipe *, void *arg)
	{
		static_cast<Link *>(arg)->in_done(urb);
	}

	void out_kick();
	void out_done(struct usbd_urb *urb);
	int  in_post();
	void in_done(struct usbd_urb *urb);

	std::recursive_mutex lock_;
	struct usbd_pipe    *in_ = nullptr, *out_ = nullptr;
	Urb                  in_urb_, out_urb_;
	UsbBuf               in_buf_, out_buf_;
	unsigned             out_packet_ = 0;     // wMaxPacketSize of bulk OUT

	std::deque<std::vector<u8>> outq_;  // front: being sent from outq_off_
	usize   outq_off_ = 0, outq_bytes_ = 0;
	usize   out_len_ = 0;                    // bytes in flight
	bool     in_pending_ = false, out_pending_ = false;
	bool     closing_ = false, broken_ = false;
	u32 fail_status_ = 0;
	u64 rx_bytes_ = 0;                   // usbd thread
};

// lock_ held. Starts the next transfer if nothing is in flight.
void Link::out_kick()
{
	if (out_pending_ || outq_.empty() || closing_ || broken_)
		return;
	const std::vector<u8> &b = outq_.front();
	usize n = usb_chunk(b.size() - outq_off_, kOutBufSize, out_packet_);
	std::memcpy(out_buf_.get(), b.data() + outq_off_, n);
	usbd_setup_bulk(out_urb_.get(), URB_DIR_OUT, out_buf_.get(), (u32)n);
	out_pending_ = true;
	out_len_ = n;
	int rc = usbd_io(out_urb_.get(), out_, out_done_cb, this, USBD_TIME_INFINITY);
	if (rc != EOK) {
		out_pending_ = false;
		broken_ = true;
		log_msg("usb: bulk OUT usbd_io: %s", strerror(rc));
		wake();
	}
}

bool Link::send(const u8 *p, usize n)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	if (broken_ || closing_)
		return false;
	if (outq_bytes_ + n > kOutQueueMax) {
		log_msg("usb: out queue full (%zu bytes waiting), %zu bytes not sent", outq_bytes_, n);
		return false;
	}
	outq_.emplace_back(p, p + n);
	outq_bytes_ += n;
	out_kick();
	return true;
}

void Link::out_done(struct usbd_urb *urb)
{
	u32 len;
	bool ok = urb_ok(urb, &len);

	std::lock_guard<std::recursive_mutex> lock(lock_);
	out_pending_ = false;
	if (closing_)
		return;
	if (!ok) {
		fail_status_ = urb_status(urb);
		log_msg("usb: bulk OUT failed, status 0x%08x", (unsigned)fail_status_);
		broken_ = true;
		wake();
		return;
	}
	outq_off_ += out_len_;
	outq_bytes_ -= out_len_;
	if (outq_off_ == outq_.front().size()) {
		outq_.pop_front();
		outq_off_ = 0;
	}
	out_kick();
}

// lock_ held.
int Link::in_post()
{
	usbd_setup_bulk(in_urb_.get(), URB_DIR_IN | URB_SHORT_XFER_OK, in_buf_.get(), kInBufSize);
	in_pending_ = true;
	int rc = usbd_io(in_urb_.get(), in_, in_done_cb, this, USBD_TIME_INFINITY);
	if (rc != EOK) {
		in_pending_ = false;
		log_msg("usb: bulk IN usbd_io: %s", strerror(rc));
	}
	return rc;
}

void Link::in_done(struct usbd_urb *urb)
{
	u32 len;
	bool ok = urb_ok(urb, &len);

	name_usbd_thread();
	{
		std::lock_guard<std::recursive_mutex> lock(lock_);
		in_pending_ = false;
		if (closing_)
			return;
		if (!ok) {
			fail_status_ = urb_status(urb);
			log_msg("usb: bulk IN failed, status 0x%08x", (unsigned)fail_status_);
			broken_ = true;
			wake();
			return;
		}
	}
	if (len > kInBufSize)
		len = kInBufSize;
	rx_bytes_ += len;
	if (len > 0)
		client->rx(in_buf_.get(), len);           // may send()
	std::lock_guard<std::recursive_mutex> lock(lock_);
	if (!closing_ && in_post() != EOK) {
		broken_ = true;
		wake();
	}
}

void Link::dispose(std::unique_ptr<Link> l)
{
	if ((l->in_ != nullptr && close_pipe_wait(l->in_) != EOK) ||
	    (l->out_ != nullptr && close_pipe_wait(l->out_) != EOK)) {
		log_msg("usb: pipe still busy, leaking the link");
		l.release();
	}
}

void Link::close(std::unique_ptr<Link> l)
{
	unsigned t0 = log_ms();
	bool pending;

	{
		std::lock_guard<std::recursive_mutex> lock(l->lock_);
		l->closing_ = true;
	}
	if (l->in_ != nullptr)
		usbd_abort_pipe(l->in_);
	if (l->out_ != nullptr)
		usbd_abort_pipe(l->out_);
	for (;;) {
		{
			std::lock_guard<std::recursive_mutex> lock(l->lock_);
			pending = l->in_pending_ || l->out_pending_;
		}
		if (!pending || log_ms() - t0 > kTeardownWaitMs)
			break;
		usleep(10 * 1000);
	}
	if (pending) {
		log_msg("usb: transfers still pending after %u ms, leaking the link", kTeardownWaitMs);
		l.release();
		return;
	}
	dispose(std::move(l));
	log_msg("usb: link closed (%u ms)", log_ms() - t0);
}

std::unique_ptr<Link> Link::open(struct usbd_device *dev)
{
	usbd_descriptors_t *cfg_d, *ifc, *ep, *ep_in = nullptr, *ep_out = nullptr;
	struct usbd_desc_node *dnode = nullptr, *cnode, *inode;

	if (usbd_device_descriptor(dev, &dnode) == nullptr) {
		log_msg("usb: accessory: no device descriptor");
		return nullptr;
	}
	for (int c = 0; ep_in == nullptr || ep_out == nullptr; c++) {
		cfg_d = usbd_parse_descriptors(dev, dnode, USB_DESC_CONFIGURATION, c, &cnode);
		if (cfg_d == nullptr)
			break;
		for (int i = 0; ep_in == nullptr || ep_out == nullptr; i++) {
			ifc = usbd_parse_descriptors(dev, cnode, USB_DESC_INTERFACE, i, &inode);
			if (ifc == nullptr)
				break;
			log_msg("usb:   config %u iface %u: class %02x/%02x/%02x, %u endpoints",
			        cfg_d->configuration.bConfigurationValue,
			        ifc->interface.bInterfaceNumber, ifc->interface.bInterfaceClass,
			        ifc->interface.bInterfaceSubClass, ifc->interface.bInterfaceProtocol,
			        ifc->interface.bNumEndpoints);
			// accessory = ff/ff; the adb interface of 2d01 is ff/42
			if (ifc->interface.bInterfaceClass != 0xff ||
			    ifc->interface.bInterfaceSubClass != 0xff)
				continue;
			ep_in = ep_out = nullptr;
			for (int e = 0; (ep = usbd_parse_descriptors(dev, inode, USB_DESC_ENDPOINT,
			                                             e, nullptr)) != nullptr; e++) {
				if ((ep->endpoint.bmAttributes & 3) != USB_ATTRIB_BULK)
					continue;
				if (ep->endpoint.bEndpointAddress & USB_ENDPOINT_IN) {
					if (ep_in == nullptr)
						ep_in = ep;
				} else if (ep_out == nullptr) {
					ep_out = ep;
				}
			}
		}
	}
	if (ep_in == nullptr || ep_out == nullptr) {
		log_msg("usb: accessory: no ff/ff interface with bulk IN + OUT");
		return nullptr;
	}
	log_msg("usb: accessory endpoints IN 0x%02x (%u B) OUT 0x%02x (%u B)",
	        ep_in->endpoint.bEndpointAddress, ep_in->endpoint.wMaxPacketSize,
	        ep_out->endpoint.bEndpointAddress, ep_out->endpoint.wMaxPacketSize);

	std::unique_ptr<Link> l(new (std::nothrow) Link());
	if (!l)
		return nullptr;
	l->out_packet_ = ep_out->endpoint.wMaxPacketSize;
	int rc = usbd_open_pipe(dev, ep_in, &l->in_);
	if (rc == EOK)
		rc = usbd_open_pipe(dev, ep_out, &l->out_);
	if (rc != EOK) {
		log_msg("usb: usbd_open_pipe: %s", strerror(rc));
		dispose(std::move(l));
		return nullptr;
	}
	l->in_urb_.reset(usbd_alloc_urb(nullptr));
	l->out_urb_.reset(usbd_alloc_urb(nullptr));
	l->in_buf_ = usb_alloc(kInBufSize);
	l->out_buf_ = usb_alloc(kOutBufSize);
	if (!l->in_urb_ || !l->out_urb_ || !l->in_buf_ || !l->out_buf_) {
		log_msg("usb: out of USB memory");
		dispose(std::move(l));
		return nullptr;
	}
	{
		std::lock_guard<std::recursive_mutex> lock(l->lock_);
		rc = l->in_post();
	}
	if (rc != EOK) {
		dispose(std::move(l));
		return nullptr;
	}
	return l;
}

// --- AOA handshake -------------------------------------------------------

const char *status_name(u32 st)
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

int ctrl(struct usbd_pipe *pipe, struct usbd_urb *urb, bool in, u8 req, u16 index,
         u8 *buf, u32 len, u32 *got)
{
	u16 rtype = USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE |
	                 (in ? USB_DIRECTION_HOST : USB_DIRECTION_DEVICE);
	unsigned t0 = log_ms();

	usbd_setup_vendor(urb, in ? URB_DIR_IN : URB_DIR_OUT, req, rtype, 0, index, buf, len);
	int rc = usbd_io(urb, pipe, nullptr, nullptr, kCtrlTimeoutMs);
	if (rc == EOK && !urb_ok(urb, got)) {
		u32 st = urb_status(urb);

		log_msg("usb:   request %u index %u: status 0x%08x (%s) after %u ms", req, index,
		        (unsigned)st, status_name(st), log_ms() - t0);
		rc = EIO;
	}
	return rc;
}

enum class Switch {
	Started,  // START sent (phone will re-enumerate)
	NotAoa,   // it refused GET_PROTOCOL
	Failed,   // a request failed some other way: worth another try
};

Switch aoa_switch(struct usbd_device *dev)
{
	struct usbd_pipe *pipe = nullptr;
	usbd_descriptors_t *dd = usbd_parse_descriptors(dev, nullptr, USB_DESC_DEVICE, 0, nullptr);

	if (dd == nullptr || usbd_open_pipe(dev, dd, &pipe) != EOK) {
		log_msg("usb:   no control pipe");
		return Switch::Failed;
	}
	struct PipeClose {
		struct usbd_pipe *pipe;
		~PipeClose() { close_pipe_wait(pipe); }
	} pipe_close{pipe};
	Urb urb(usbd_alloc_urb(nullptr));
	UsbBuf usb_buf = usb_alloc(256);
	if (!urb || !usb_buf)
		return Switch::Failed;
	u8 *buf = usb_buf.get();
	u32 got = 0;

	buf[0] = buf[1] = 0;
	int rc = ctrl(pipe, urb.get(), true, kAoaGetProtocol, 0, buf, 2, &got);
	int version = buf[0] | (buf[1] << 8);
	log_msg("usb:   AOA GET_PROTOCOL: %s, %u bytes, version %d",
	        rc ? strerror(rc) : "ok", (unsigned)got, version);
	if (rc != EOK || got < 2 || version < 1) {
		// no answer at all, not a refusal: try again
		bool no_answer = rc == EIO &&
		                 (urb_status(urb.get()) & ~USBD_URB_STATUS_MASK) != USBD_STATUS_STALL;
		return no_answer ? Switch::Failed : Switch::NotAoa;
	}
	for (int i = 0; i < 6; i++) {
		usize n = std::strlen(cfg.strings[i]) + 1;

		if (n > 256)
			n = 256;
		std::memcpy(buf, cfg.strings[i], n);
		buf[n - 1] = 0;
		rc = ctrl(pipe, urb.get(), false, kAoaSendString, (u16)i, buf, (u32)n, &got);
		if (rc != EOK) {
			log_msg("usb:   AOA SEND_STRING %d: %s", i, strerror(rc));
			return Switch::Failed;
		}
	}
	rc = ctrl(pipe, urb.get(), false, kAoaStart, 0, buf, 0, &got);
	log_msg("usb:   AOA START: %s", rc ? strerror(rc) : "ok");
	return rc == EOK ? Switch::Started : Switch::Failed;
}

// --- device bookkeeping (manager thread only) ----------------------------

struct DevEntry {
	bool                   used = false;
	u8                path = 0, devno = 0;
	u16               generation = 0;
	struct usbd_device    *dev = nullptr;   // still attached, or nullptr
	usbd_device_instance_t inst{};          // the instance we attached through
	bool                   is_acc = false;  // an AOA accessory (18d1:2d0x)
	unsigned               reopen_at = 0;   // log_ms() to reopen the accessory, 0 = no
	int                    reopen_tries = 0;
	unsigned               inserted_at = 0; // log_ms() of the insertion
	unsigned               handshake_at = 0; // log_ms() of the next AOA handshake, 0 = no
	int                    handshake_tries = 0;
	bool                   retryable = false; // a handshake failed in a way worth retrying
};

struct usbd_connection *conn;
DevEntry devs[kMaxDevices];
std::mutex acc_lock;                  // acc itself, against send() from other threads
std::unique_ptr<Link> acc;            // the accessory link, or nullptr
DevEntry *acc_entry;
unsigned switch_since;                // log_ms() of AOA START, 0 = none

unsigned due_in(unsigned ms)
{
	unsigned t = log_ms() + ms;
	return t ? t : 1;
}

DevEntry *dev_find(const usbd_device_instance_t *in)
{
	for (DevEntry &d : devs)
		if (d.used && d.path == in->path && d.devno == in->devno &&
		    d.generation == in->generation)
			return &d;
	return nullptr;
}

void dev_release(DevEntry *d)
{
	// A device we fail to detach stays "open" in io-usb, which then holds
	// its port off even after an unplug (CLASS_ExtractDevice), so retry
	// while libusbdi still counts I/O as pending.
	if (d->dev != nullptr) {
		unsigned t0 = log_ms();
		int rc;

		while ((rc = usbd_detach(d->dev)) == EBUSY && log_ms() - t0 < kTeardownWaitMs)
			usleep(10 * 1000);
		if (rc != EOK)
			log_msg("usb:   usbd_detach: %s", strerror(rc));
		d->dev = nullptr;
	}
}

void link_drop(const char *why, State next)
{
	std::unique_ptr<Link> l;
	{
		std::lock_guard<std::mutex> lock(acc_lock);
		l = std::move(acc);
	}
	if (!l)
		return;
	log_msg("usb: accessory link down: %s", why);
	Link::close(std::move(l));
	client->closed(why);
	if (acc_entry != nullptr)
		dev_release(acc_entry);
	acc_entry = nullptr;
	set_state(next, why);
}

bool acc_open(DevEntry *d)
{
	std::unique_ptr<Link> l = Link::open(d->dev);
	if (!l)
		return false;
	{
		std::lock_guard<std::mutex> lock(acc_lock);
		acc = std::move(l);
	}
	acc_entry = d;
	set_state(State::Open, "accessory link open");
	client->opened();
	return true;
}

// The link broke but the device was not removed (yet). Seen when another
// client of the same device detaches - io-usb's udi_detach aborts every URB
// queued on the interface's endpoints, ours included - or when the media
// launcher's MTP probe resets the device. An unplug also aborts our URBs
// first; its removal event follows and cancels the reopen.
void acc_broken()
{
	DevEntry *d = acc_entry;
	u32 status = acc->fail_status();
	u64 rx = acc->rx_bytes();

	log_msg("usb: accessory link broken (status 0x%08x, %llu bytes received)",
	        (unsigned)status, (unsigned long long)rx);
	link_drop("USB link interrupted - reconnecting", State::Waiting);
	if (d != nullptr) {
		if (rx > 0)
			d->reopen_tries = 0;      // it worked for a while: fresh retries
		d->reopen_at = due_in(kReopenDelayMs);
	}
}

void acc_reopen_due()
{
	unsigned now = log_ms();

	for (int i = 0; i < kMaxDevices && !acc; i++) {
		DevEntry *d = &devs[i];

		if (!d->used || d->reopen_at == 0 || (int)(now - d->reopen_at) < 0)
			continue;
		d->reopen_at = 0;
		if (++d->reopen_tries > kReopenMax) {
			log_msg("usb: accessory reopen: giving up after %d tries", kReopenMax);
			set_state(State::Waiting, "USB link keeps failing - replug the phone");
			continue;
		}
		int rc = usbd_attach(conn, &d->inst, 0, &d->dev);
		log_msg("usb: accessory reopen %d/%d: attach %s", d->reopen_tries, kReopenMax,
		        rc ? strerror(rc) : "ok");
		if (rc != EOK) {
			d->dev = nullptr;         // gone: its removal event will follow
			continue;
		}
		if (!acc_open(d)) {
			dev_release(d);
			d->reopen_at = due_in(kReopenDelayMs * (unsigned)d->reopen_tries);
		}
	}
}

// Attempt the AOA handshake on a (non-accessory) device whose time has come.
// A phone can answer GET_PROTOCOL (v2) and two strings, then fail
// SEND_STRING 2 with "device did not answer"; giving it up for good loses
// it. So a device that has shown it speaks AOA gets more tries.
void handshake_due()
{
	unsigned now = log_ms();

	for (DevEntry &entry : devs) {
		DevEntry *d = &entry;

		if (!d->used || d->handshake_at == 0 || (int)(now - d->handshake_at) < 0)
			continue;
		d->handshake_at = 0;
		d->handshake_tries++;
		int rc = usbd_attach(conn, &d->inst, 0, &d->dev);
		if (rc != EOK) {
			log_msg("usb: AOA handshake %d/%d: attach %s", d->handshake_tries,
			        kHandshakeTries, strerror(rc));
			d->dev = nullptr;
			continue;
		}
		log_msg("usb: AOA handshake %d/%d on %04x:%04x, %u ms after insertion",
		        d->handshake_tries, kHandshakeTries, (unsigned)d->inst.ident.vendor,
		        (unsigned)d->inst.ident.device, now - d->inserted_at);
		Switch r = aoa_switch(d->dev);
		dev_release(d);               // keep the entry until it is removed
		if (r == Switch::Failed)
			d->retryable = true;
		if (r == Switch::Started) {
			switch_since = due_in(0);
			set_state(State::Switching, "phone is switching to accessory mode");
			continue;
		}
		if (r == Switch::NotAoa && !d->retryable) {
			log_msg("usb:   not an Android (AOA) device, ignored");
			continue;
		}
		if (d->handshake_tries >= kHandshakeTries) {
			log_msg("usb:   AOA handshake failed %d times, giving up", kHandshakeTries);
			set_state(State::Waiting, "accessory handshake failed - replug the phone");
			continue;
		}
		d->handshake_at = due_in(kHandshakeDelayMs * (unsigned)d->handshake_tries);
		log_msg("usb:   AOA handshake failed, retrying in %u ms",
		        kHandshakeDelayMs * (unsigned)d->handshake_tries);
		if (d->handshake_tries == 1)
			slog_snapshot("first handshake failure");
	}
}

void on_insert(usbd_device_instance_t *in)
{
	DevEntry *d = dev_find(in);
	struct usbd_desc_node *node = nullptr;
	char detail[80];

	if (d != nullptr)                 // another interface of a known device
		return;
	// An accessory arrives once per interface. Attach through the accessory
	// interface (ff/ff), not adb (ff/42) or audio: sdl_core's QNX transport
	// filters its AOA connection the same way.
	if (in->ident.vendor == kAoaVid && in->ident.device >= kAoaPidMin &&
	    in->ident.device <= kAoaPidMax && in->ident.dclass == 0xff &&
	    in->ident.subclass != 0xff) {
		log_msg("usb: insert %04x:%04x iface %u class ff/%02x: not the accessory "
		        "interface, skipped", (unsigned)in->ident.vendor, (unsigned)in->ident.device,
		        (unsigned)in->iface, (unsigned)in->ident.subclass);
		return;
	}
	int i;
	for (i = 0; i < kMaxDevices && devs[i].used; i++)
		;
	if (i == kMaxDevices) {
		log_msg("usb: too many devices, insert ignored");
		return;
	}
	d = &devs[i];
	*d = DevEntry();
	d->used = true;
	d->path = in->path;
	d->devno = in->devno;
	d->generation = in->generation;
	d->inst = *in;
	d->inserted_at = log_ms();
	int rc = usbd_attach(conn, in, 0, &d->dev);
	log_msg("usb: insert path %u devno %u gen %u iface %u, %04x:%04x class %02x/%02x/%02x: "
	        "attach %s", in->path, in->devno, in->generation, (unsigned)in->iface,
	        (unsigned)in->ident.vendor, (unsigned)in->ident.device,
	        (unsigned)in->ident.dclass, (unsigned)in->ident.subclass,
	        (unsigned)in->ident.protocol, rc ? strerror(rc) : "ok");
	if (rc != EOK) {
		d->dev = nullptr;
		return;
	}
	usbd_device_descriptor_t *dd = usbd_device_descriptor(d->dev, &node);
	if (dd == nullptr) {
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
	if (dd->idVendor == kAoaVid && dd->idProduct >= kAoaPidMin && dd->idProduct <= kAoaPidMax) {
		if (acc) {
			log_msg("usb:   a second accessory, ignored");
			dev_release(d);
			return;
		}
		switch_since = 0;
		d->is_acc = true;
		if (!acc_open(d)) {
			dev_release(d);
			set_state(State::Waiting, "accessory open failed - replug the phone");
		}
		return;
	}
	// Stay off EP0 until the media launcher is done with the device. Its
	// insertion callback (vdev-medialaunch vml_insertion, P1001) runs at
	// the same moment as ours: an MTP probe, GET_DESCRIPTOR(0xEE) on EP0,
	// that calls usbd_reset_device on any failure but a STALL
	// (0x080539c0), then usbd_select_config (SET_CONFIGURATION), then
	// the bus event thread waits its -c delay (default 2000 ms, srv-starter
	// passes none) before it starts the MTP driver. A handshake that
	// runs inside that window (+0.2 s, +1.3 s) fails: GET_PROTOCOL
	// is answered, then DEV_NOANSWER on the next request and on everything
	// after it - a reset device - and io-usb reported the phone removed
	// 3-5 s later. The handshake waits kHandshakeFirstMs. If the
	// phone is reset anyway, its re-insertion starts over with a new entry.
	std::snprintf(detail, sizeof(detail), "USB device %04x:%04x found - handshake in %u s",
	              (unsigned)dd->idVendor, (unsigned)dd->idProduct, kHandshakeFirstMs / 1000);
	dev_release(d);                   // keep the entry until it is removed; frees dd
	d->handshake_at = due_in(kHandshakeFirstMs);
	set_state(State::Waiting, detail);
}

// The system log at this moment, into <sd>/var/usb_slog_<n>.txt, in the
// background (sloginfo + SD write take about a second): io-usb's and the
// media launcher's side of a failed handshake or a removal.
void slog_snapshot(const char *why)
{
	static int n;
	char cmd[400];

	if (cfg.sd_path == nullptr || n >= kSlogSnapshots)
		return;
	n++;
	std::snprintf(cmd, sizeof(cmd), "sloginfo > '%s/var/usb_slog_%d.txt' 2>&1 &", cfg.sd_path, n);
	log_msg("usb: slog snapshot %d (%s) -> var/usb_slog_%d.txt", n, why, n);
	if (std::system(cmd) != 0)
		log_msg("usb:   could not start sloginfo");
}

void on_remove(usbd_device_instance_t *in)
{
	DevEntry *d = dev_find(in);

	if (d == nullptr)                 // other interfaces of a gone device
		return;
	log_msg("usb: remove path %u devno %u gen %u", in->path, in->devno, in->generation);
	slog_snapshot("device removed");
	if (d == acc_entry)
		link_drop("phone disconnected", State::Waiting);
	else if (d->is_acc)               // broken earlier, reopen pending
		set_state(State::Waiting, "phone disconnected");
	dev_release(d);
	d->used = false;
}

// --- manager thread ------------------------------------------------------

bool try_connect(unsigned attempt)
{
	static usbd_device_ident_t ident = {
		USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD,
		USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD,
	};
	static usbd_funcs_t funcs = { _USBDI_NFUNCS, cb_insertion, cb_removal, nullptr };
	usbd_connect_parm_t parm;

	std::memset(&parm, 0, sizeof(parm));
	parm.vusb = USB_VERSION;
	parm.vusbd = USBD_VERSION;
	parm.ident = &ident;
	parm.funcs = &funcs;
	int rc = usbd_connect(&parm, &conn);
	if (rc == EOK) {
		log_msg("usb: usbd_connect ok (attempt %u)", attempt);
		set_state(State::Waiting, "plug the phone into the car's USB port");
		return true;
	}
	conn = nullptr;
	if (attempt == 1 || attempt % 30 == 0) {
		char detail[80];

		std::snprintf(detail, sizeof(detail), "usbd_connect: %s - retrying", strerror(rc));
		log_msg("usb: %s (attempt %u)", detail, attempt);
		set_state(State::NoStack, detail);
	}
	return false;
}

void wait_locked(std::unique_lock<std::mutex> &lock, unsigned ms)
{
	qcond.wait_ms(lock, ms, [] { return !queue.empty() || kick || stopping; });
	kick = false;
}

void manager()
{
	unsigned attempt = 0, last_tick = 0;

	cpumon_name_thread("usb");
	for (;;) {
		Event ev;
		bool have;
		{
			std::unique_lock<std::mutex> lock(qlock);
			if (stopping)
				break;
			if (conn == nullptr) {
				lock.unlock();
				if (!try_connect(++attempt)) {
					lock.lock();
					wait_locked(lock, kConnectRetryMs);
				}
				continue;
			}
			wait_locked(lock, cfg.tick_ms);
			have = !queue.empty();
			if (have) {
				ev = queue.front();
				queue.pop_front();
			}
		}

		if (have) {
			if (ev.kind == EvKind::Insert)
				on_insert(&ev.inst);
			else
				on_remove(&ev.inst);
		}
		if (acc && acc->broken())
			acc_broken();
		if (!acc)
			acc_reopen_due();
		handshake_due();
		if (switch_since != 0 && log_ms() - switch_since > kSwitchTimeoutMs) {
			switch_since = 0;
			set_state(State::Waiting, "phone did not come back as an accessory - replug it");
		}
		if (log_ms() - last_tick >= cfg.tick_ms) {
			last_tick = log_ms();
			client->tick();
		}
	}

	log_msg("usb: manager stopping");
	link_drop("stopping", State::Waiting);
	for (DevEntry &d : devs)
		if (d.used)
			dev_release(&d);
	if (conn != nullptr) {
		int rc = usbd_disconnect(conn);

		log_msg("usb: usbd_disconnect: %s", rc ? strerror(rc) : "ok");
		conn = nullptr;
	}
}

} // namespace

const char *state_name(State s)
{
	switch (s) {
	case State::NoStack:   return "no USB stack";
	case State::Waiting:   return "waiting";
	case State::Switching: return "switching";
	case State::Open:      return "open";
	}
	return "?";
}

bool send(const u8 *p, usize n)
{
	std::lock_guard<std::mutex> lock(acc_lock);
	return acc && acc->send(p, n);
}

usize out_pending()
{
	std::lock_guard<std::mutex> lock(acc_lock);
	return acc ? acc->pending() : 0;
}

bool start(const Config &c, Client &cl)
{
	cfg = c;
	client = &cl;
	try {
		manager_thread = std::thread(manager);
	} catch (const std::system_error &e) {
		log_msg("usb: manager thread: %s", e.what());
		return false;
	}
	manager_started = true;
	return true;
}

void stop()
{
	if (!manager_started)
		return;
	{
		std::lock_guard<std::mutex> lock(qlock);
		stopping = true;
	}
	qcond.notify();
	manager_thread.join();
	manager_started = false;
}

} // namespace aoa_host

// aoa.cpp - Android Open Accessory host on io-usb (see aoa.h).
//
// USBDI usage follows smartdevicelink's QNX transport (usb_handler.cc /
// usb_connection.cc), which shipped on QNX head units: synchronous vendor
// control transfers on the default pipe for the AOA handshake, asynchronous
// bulk transfers for data, a short transfer reported as
// USBD_STATUS_CMP_ERR | USBD_STATUS_DATA_UNDERRUN counting as success.
#include "aoa.h"

#include <atomic>
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
#include <utility>

#include <pthread.h>
#include <unistd.h>

#define _Static_assert static_assert   // the SDK's usbdi.h checks its layouts the C way
#include <sys/usbdi.h>
#undef _Static_assert

extern "C" {
#include "cpumon.h"
#include "log.h"
}

#include "ack_batch.h"
#include "hwvideo.h"
#include "model.h"
#include "mono_cond.h"
#include "vasm.h"
#include "video.h"

using model::LinkState;
using proto::PicStatus;

namespace aoa {

namespace {

// AOA protocol (source.android.com/docs/core/interaction/accessories/aoa)
constexpr uint8_t  kAoaGetProtocol = 51;
constexpr uint8_t  kAoaSendString = 52;
constexpr uint8_t  kAoaStart = 53;
constexpr unsigned kAoaVid = 0x18D1;
constexpr unsigned kAoaPidMin = 0x2D00;   // 2d00 accessory, 2d01 +adb, 2d04/05 +audio
constexpr unsigned kAoaPidMax = 0x2D05;

// Identification sent to the phone; the Android app's accessory_filter.xml
// matches manufacturer + model.
constexpr const char *kAoaStrings[6] = {
	"mmi3g",
	"audi_stream_receiver",
	"MMI 3G+ Audi stream receiver",
	"1",
	"https://github.com/dspl1236/MMI3G-Toolkit",
	"mmi3g-0001",
};
constexpr char kReceiverHello[] = "audi_stream_receiver-mmi/1";

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
constexpr unsigned kOutQueue = 16;
constexpr unsigned kOutFrameMax = proto::kHdr + 128;
constexpr int      kMaxDevices = 16;
constexpr size_t   kMaxEvents = 32;
// The manager looks at everything this often; a batch of acks waits no longer.
constexpr unsigned kManagerTickMs = 200;

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
AckBatch          acks;
std::thread       manager_thread;
bool              manager_started;
// log_ms() + 1 of the last complete picture or frame (0: none yet): while
// pictures come, the system log is saved every kSlogPeriodMs
std::atomic<unsigned> video_rx_ms{0};
constexpr unsigned kSlogPeriodMs = 60000;
constexpr unsigned kStreamingGapMs = 5000;  // no picture for this long: not streaming

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

// --- what io-usb hands out -----------------------------------------------

struct UrbFree {
	void operator()(struct usbd_urb *u) const { usbd_free_urb(u); }
};
using Urb = std::unique_ptr<struct usbd_urb, UrbFree>;

struct UsbFree {
	void operator()(uint8_t *p) const { usbd_free(p); }
};
using UsbBuf = std::unique_ptr<uint8_t, UsbFree>;

UsbBuf usb_alloc(size_t n)
{
	return UsbBuf(static_cast<uint8_t *>(usbd_alloc(n)));
}

bool urb_ok(struct usbd_urb *urb, uint32_t *len)
{
	uint32_t status = 0;

	*len = 0;
	int rc = usbd_urb_status(urb, &status, len);
	if (rc != EOK && rc != EIO)
		return false;
	return (status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP ||
	       status == (USBD_STATUS_CMP_ERR | USBD_STATUS_DATA_UNDERRUN);
}

uint32_t urb_status(struct usbd_urb *urb)
{
	uint32_t status = 0, len = 0;

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

// --- the accessory connection --------------------------------------------

// Printable copy of a payload for the log: control chars as \n, \xNN.
void log_text(const char *what, const uint8_t *s, unsigned n)
{
	static char buf[4 * proto::kMaxPayload + 1];
	unsigned o = 0;

	for (unsigned i = 0; i < n && o + 5 < sizeof(buf); i++) {
		unsigned char c = s[i];

		if (c == '\n') {
			buf[o++] = '\\';
			buf[o++] = 'n';
		} else if (c < 0x20 || c == 0x7f) {
			o += (unsigned)std::snprintf(buf + o, sizeof(buf) - o, "\\x%02x", c);
		} else {
			buf[o++] = (char)c;
		}
	}
	buf[o] = 0;
	log_msg("%s len=%u: %s", what, n, buf);
}

class Link {
public:
	// Finds the accessory interface's bulk endpoints and opens the link;
	// nullptr if that fails.
	static std::unique_ptr<Link> open(struct usbd_device *dev);

	// Queues a frame; false if the queue is full.
	bool send(uint8_t type, const void *payload, unsigned len);

	bool broken()
	{
		std::lock_guard<std::recursive_mutex> lock(lock_);
		return broken_;
	}
	uint32_t fail_status()
	{
		std::lock_guard<std::recursive_mutex> lock(lock_);
		return fail_status_;
	}
	int rx_frames() const { return rx_frames_; }

	// Frees the link, unless a pipe will not close: then a URB may still be
	// in flight into our buffers, so everything is leaked instead.
	static void dispose(std::unique_ptr<Link> l);
	// Aborts both pipes and waits for the completions, then frees. If io-usb
	// never completes them the link is leaked rather than freed under it.
	static void close(std::unique_ptr<Link> l);

private:
	Link() : assembler_(video::kMaxBytes) {}

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
	void send_caps();
	void set_peer(const uint8_t *p, unsigned n);
	void on_frame(uint8_t type, const uint8_t *p, unsigned n);
	void on_video(uint8_t type, const uint8_t *p, unsigned n);

	proto::Decoder decoder_;
	Assembler      assembler_;           // the picture being received
	unsigned       rx_start_us_ = 0;     // of that picture

	struct usbd_pipe   *in_ = nullptr, *out_ = nullptr;
	Urb                 in_urb_, out_urb_;
	UsbBuf              in_buf_, out_buf_;

	// Recursive: usbd_io() is called with the lock held, and should io-usb
	// ever run the completion callback inline it takes the lock again.
	std::recursive_mutex lock_;           // out queue + flags below
	uint8_t  outq_[kOutQueue][kOutFrameMax];
	unsigned outq_len_[kOutQueue];
	unsigned outq_head_ = 0, outq_n_ = 0;
	bool     in_pending_ = false, out_pending_ = false;
	bool     closing_ = false, broken_ = false;
	uint32_t fail_status_ = 0;            // URB status that broke it

	bool             hello_seen_ = false; // usbd thread
	std::atomic<int> rx_frames_{0};
};

// lock_ held. Starts the next queued frame if nothing is in flight.
void Link::out_kick()
{
	if (out_pending_ || outq_n_ == 0 || closing_ || broken_)
		return;
	unsigned n = outq_len_[outq_head_];
	std::memcpy(out_buf_.get(), outq_[outq_head_], n);
	usbd_setup_bulk(out_urb_.get(), URB_DIR_OUT, out_buf_.get(), n);
	out_pending_ = true;
	int rc = usbd_io(out_urb_.get(), out_, out_done_cb, this, USBD_TIME_INFINITY);
	if (rc != EOK) {
		out_pending_ = false;
		broken_ = true;
		log_msg("usb: bulk OUT usbd_io: %s", strerror(rc));
		wake();
	}
}

bool Link::send(uint8_t type, const void *payload, unsigned len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	if (outq_n_ == kOutQueue) {
		log_msg("usb: out queue full, frame type %u not sent", type);
		return false;
	}
	unsigned slot = (outq_head_ + outq_n_) % kOutQueue;
	unsigned n = proto::encode(outq_[slot], kOutFrameMax, type, payload, len);
	if (n > 0) {
		outq_len_[slot] = n;
		outq_n_++;
		out_kick();
	}
	return true;
}

void Link::out_done(struct usbd_urb *urb)
{
	uint32_t len;
	bool ok = urb_ok(urb, &len);

	std::lock_guard<std::recursive_mutex> lock(lock_);
	out_pending_ = false;
	if (closing_)
		return;
	if (ok) {
		outq_head_ = (outq_head_ + 1) % kOutQueue;
		outq_n_--;
		out_kick();
	} else {
		fail_status_ = urb_status(urb);
		log_msg("usb: bulk OUT failed, status 0x%08x", (unsigned)fail_status_);
		broken_ = true;
		wake();
	}
}

void Link::send_caps()
{
	auto caps = video::caps();
	send(proto::kVideoCaps, caps.data(), (unsigned)caps.size());
}

// VIDEO_START / VIDEO_DATA; usbd thread. Bad pictures are acked at once,
// JPEGs when they are on screen (video.cpp), H.264 frames with a batch.
void Link::on_video(uint8_t type, const uint8_t *p, unsigned n)
{
	Assembler::Result r;

	if (type == proto::kVideoStart) {
		r = assembler_.start(p, n);
		if (r.rx == Assembler::Rx::Ok)
			rx_start_us_ = log_us();
		if (r.cut_short >= 0) {
			log_msg("video: picture %d cut short by the next one", r.cut_short);
			ack(PicStatus::Dropped);
		}
	} else {
		r = assembler_.data(p, n);
	}
	if (r.rx == Assembler::Rx::Bad) {
		log_msg("rx %s for picture %d refused (%u bytes%s)",
		        type == proto::kVideoStart ? "VIDEO_START" : "VIDEO_DATA", r.id, n,
		        n >= proto::kVideoStartLen && type == proto::kVideoStart ?
		        ": bad size/format/dimensions" : "");
		ack(PicStatus::Bad);
	} else if (r.rx == Assembler::Rx::Done) {
		Picture pic = assembler_.take();
		video_rx_ms = log_ms() + 1;
		pic.rx_start_us = rx_start_us_;
		pic.rx_done_us = log_us();
		if (pic.format == proto::kFmtH264) {
			// into the hardware stream (proto.h)
			ack_later(hwvideo::frame(pic.data.get(), pic.size));
			return;
		}
		if (hwvideo::active())
			hwvideo::stop("the phone sent a picture: back to pictures");
		video::submit(std::move(pic));
	}
}

void Link::set_peer(const uint8_t *p, unsigned n)
{
	char detail[model::kDetailMax + 1];

	if (n > model::kDetailMax)
		n = model::kDetailMax;
	std::memcpy(detail, p, n);
	detail[n] = 0;
	hello_seen_ = true;
	model::set_state(LinkState::Connected, detail);
}

// Runs on the usbd thread (from in_done).
void Link::on_frame(uint8_t type, const uint8_t *p, unsigned n)
{
	static const uint8_t kPhoneApp[] = "phone app";

	rx_frames_++;
	switch (type) {
	case proto::kHello:
		log_text("rx HELLO", p, n);
		send(proto::kHelloReply, kReceiverHello, sizeof(kReceiverHello) - 1);
		send_caps();
		set_peer(p, n);
		break;
	case proto::kHelloReply:
		log_text("rx HELLO_REPLY", p, n);
		set_peer(p, n);
		break;
	case proto::kVideoStart:
	case proto::kVideoData:
		on_video(type, p, n);
		if (!hello_seen_)
			set_peer(kPhoneApp, sizeof(kPhoneApp) - 1);
		break;
	case proto::kVideoStop:
		log_msg("rx VIDEO_STOP");
		assembler_.reset();
		video::stop("the phone stopped streaming");
		hwvideo::stop("the phone stopped streaming");
		break;
	case proto::kH264Config:
		log_msg("rx H264_CONFIG, %u bytes", n);
		hwvideo::config(p, n);
		if (!hello_seen_)
			set_peer(kPhoneApp, sizeof(kPhoneApp) - 1);
		break;
	case proto::kVideoDetails:
		if (n >= proto::kVideoDetailsLen)
			model::set_details(p[0] != 0);
		break;
	default:
		log_msg("rx unknown frame type 0x%02x, %u bytes, ignored", type, n);
		break;
	}
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
	uint32_t len;
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
	decoder_.feed(in_buf_.get(), len, [this](uint8_t type, const uint8_t *p, unsigned n) {
		on_frame(type, p, n);                 // may send()
	});

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
	usbd_descriptors_t *cfg, *ifc, *ep, *ep_in = nullptr, *ep_out = nullptr;
	struct usbd_desc_node *dnode = nullptr, *cnode, *inode;

	if (usbd_device_descriptor(dev, &dnode) == nullptr) {
		log_msg("usb: accessory: no device descriptor");
		return nullptr;
	}
	for (int c = 0; ep_in == nullptr || ep_out == nullptr; c++) {
		cfg = usbd_parse_descriptors(dev, dnode, USB_DESC_CONFIGURATION, c, &cnode);
		if (cfg == nullptr)
			break;
		for (int i = 0; ep_in == nullptr || ep_out == nullptr; i++) {
			ifc = usbd_parse_descriptors(dev, cnode, USB_DESC_INTERFACE, i, &inode);
			if (ifc == nullptr)
				break;
			log_msg("usb:   config %u iface %u: class %02x/%02x/%02x, %u endpoints",
			        cfg->configuration.bConfigurationValue,
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
	l->out_buf_ = usb_alloc(kOutFrameMax);
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
	l->send(proto::kHello, kReceiverHello, sizeof(kReceiverHello) - 1);
	l->send_caps();
	return l;
}

// --- AOA handshake -------------------------------------------------------

const char *status_name(uint32_t st)
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

int ctrl(struct usbd_pipe *pipe, struct usbd_urb *urb, bool in, uint8_t req, uint16_t index,
         uint8_t *buf, uint32_t len, uint32_t *got)
{
	uint16_t rtype = USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE |
	                 (in ? USB_DIRECTION_HOST : USB_DIRECTION_DEVICE);
	unsigned t0 = log_ms();

	usbd_setup_vendor(urb, in ? URB_DIR_IN : URB_DIR_OUT, req, rtype, 0, index, buf, len);
	int rc = usbd_io(urb, pipe, nullptr, nullptr, kCtrlTimeoutMs);
	if (rc == EOK && !urb_ok(urb, got)) {
		uint32_t st = urb_status(urb);

		log_msg("usb:   request %u index %u: status 0x%08x (%s) after %u ms", req, index,
		        (unsigned)st, status_name(st), log_ms() - t0);
		rc = EIO;
	}
	return rc;
}

enum class Switch {
	Started,  // START sent (phone will re-enumerate)
	NotAoa,   // it refused GET_PROTOCOL
	Failed,   // a request failed some other way, or the device speaks AOA but
	          // a later request failed: worth another try
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
	uint8_t *buf = usb_buf.get();
	uint32_t got = 0;

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

	// an AOA device from here on
	for (int i = 0; i < 6; i++) {
		size_t n = std::strlen(kAoaStrings[i]) + 1;

		std::memcpy(buf, kAoaStrings[i], n);
		rc = ctrl(pipe, urb.get(), false, kAoaSendString, (uint16_t)i, buf, (uint32_t)n, &got);
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
	uint8_t                path = 0, devno = 0;
	uint16_t               generation = 0;
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
std::unique_ptr<Link> acc;            // the accessory link, or nullptr
DevEntry *acc_entry;
unsigned switch_since;                // log_ms() of AOA START, 0 = none

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

void link_drop(const char *why, LinkState next)
{
	if (!acc)
		return;
	log_msg("usb: accessory link down: %s", why);
	Link::close(std::move(acc));
	video::stop("USB link down");
	hwvideo::stop("USB link down");
	if (acc_entry != nullptr)
		dev_release(acc_entry);
	acc_entry = nullptr;
	model::set_state(next, why);
}

// Open the accessory link on an attached accessory entry.
bool acc_open(DevEntry *d, const char *detail)
{
	{
		std::lock_guard<std::mutex> lock(qlock);
		acks.clear();                    // the old link's pictures
	}
	// Before Link::open(): its reads may set CONNECTED right away.
	model::set_state(LinkState::Accessory, detail);
	acc = Link::open(d->dev);
	if (!acc)
		return false;
	acc_entry = d;
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
	uint32_t status = acc->fail_status();
	int rx = acc->rx_frames();

	log_msg("usb: accessory link broken (status 0x%08x, %d frames received)",
	        (unsigned)status, rx);
	link_drop("USB link interrupted - reconnecting", LinkState::Accessory);
	if (d != nullptr) {
		if (rx > 0)
			d->reopen_tries = 0;      // it worked for a while: fresh retries
		d->reopen_at = log_ms() + kReopenDelayMs;
		if (d->reopen_at == 0)
			d->reopen_at = 1;
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
			model::set_state(LinkState::Waiting, "USB link keeps failing - replug the phone");
			continue;
		}
		int rc = usbd_attach(conn, &d->inst, 0, &d->dev);
		log_msg("usb: accessory reopen %d/%d: attach %s", d->reopen_tries, kReopenMax,
		        rc ? strerror(rc) : "ok");
		if (rc != EOK) {
			d->dev = nullptr;         // gone: its removal event will follow
			continue;
		}
		if (!acc_open(d, "USB link interrupted - reconnecting")) {
			dev_release(d);
			d->reopen_at = now + kReopenDelayMs * (unsigned)d->reopen_tries;
			if (d->reopen_at == 0)
				d->reopen_at = 1;
		}
	}
}

unsigned due_in(unsigned ms)
{
	unsigned t = log_ms() + ms;

	return t ? t : 1;
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
			d->dev = nullptr;         // gone: its removal event will follow
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
			model::set_state(LinkState::Switching, "phone is switching to accessory mode");
			continue;
		}
		if (r == Switch::NotAoa && !d->retryable) {
			log_msg("usb:   not an Android (AOA) device, ignored");
			continue;
		}
		if (d->handshake_tries >= kHandshakeTries) {
			log_msg("usb:   AOA handshake failed %d times, giving up", kHandshakeTries);
			model::set_state(LinkState::Waiting, "accessory handshake failed - replug the phone");
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
		if (!acc_open(d, "open Audi streaming on the phone")) {
			dev_release(d);
			model::set_state(LinkState::Waiting, "accessory open failed - replug the phone");
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
	// 3-5 s later. The handshake now waits kHandshakeFirstMs. If the
	// phone is reset anyway, its re-insertion starts over with a new entry.
	std::snprintf(detail, sizeof(detail), "USB device %04x:%04x found - handshake in %u s",
	              (unsigned)dd->idVendor, (unsigned)dd->idProduct, kHandshakeFirstMs / 1000);
	dev_release(d);                   // keep the entry until it is removed; frees dd
	d->handshake_at = due_in(kHandshakeFirstMs);
	model::set_state(LinkState::Waiting, detail);
}

const char *slog_sd;
std::atomic<int> slog_n{0};

} // namespace

// The system log at this moment, into <sd>/var/usb_slog_<n>.txt, in the
// background (sloginfo + SD write take about a second, ~150 KB). The run
// ends by pulling the card, so run.sh never gets to save audi_stream_receiver's
// slog; these snapshots are what shows io-usb's and the media launcher's side
// of a failed handshake (DEV_NOANSWER on SEND_STRING, cause unknown), and the
// system's side of an io-media error.
void slog_snapshot(const char *why)
{
	char cmd[400];

	if (slog_sd == nullptr)
		return;
	int n = ++slog_n;
	if (n > kSlogSnapshots)
		return;
	std::snprintf(cmd, sizeof(cmd), "sloginfo > '%s/var/usb_slog_%d.txt' 2>&1 &", slog_sd, n);
	log_msg("usb: slog snapshot %d (%s) -> var/usb_slog_%d.txt", n, why, n);
	if (std::system(cmd) != 0)
		log_msg("usb:   could not start sloginfo");
}

namespace {

// Manager thread: while streaming, the system log every kSlogPeriodMs into
// var/usb_slog_periodic_a.txt and _b.txt in turn. When the unit hangs or
// reboots, the app's log just stops; the older of the two was complete
// when it happened, the newer may be cut off by it.
void slog_periodic()
{
	static unsigned since;               // streaming since, or the last snapshot; 0: not streaming
	static bool next_b;
	char cmd[400];
	unsigned now = log_ms() + 1, rx = video_rx_ms;

	if (slog_sd == nullptr)
		return;
	if (rx == 0 || now - rx > kStreamingGapMs) {
		since = 0;
		return;
	}
	if (since == 0) {
		since = now;
		return;
	}
	if (now - since < kSlogPeriodMs)
		return;
	since = now;
	char which = next_b ? 'b' : 'a';
	next_b = !next_b;
	std::snprintf(cmd, sizeof(cmd), "sloginfo > '%s/var/usb_slog_periodic_%c.txt' 2>&1 &",
	              slog_sd, which);
	log_msg("usb: periodic slog snapshot -> var/usb_slog_periodic_%c.txt", which);
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
		link_drop("phone disconnected", LinkState::Waiting);
	else if (d->is_acc)               // broken earlier, reopen pending
		model::set_state(LinkState::Waiting, "phone disconnected");
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
		model::set_state(LinkState::Waiting, "plug the phone into the car's USB port");
		return true;
	}
	conn = nullptr;
	if (attempt == 1 || attempt % 30 == 0)
		log_msg("usb: usbd_connect: %s (attempt %u)", strerror(rc), attempt);
	char detail[model::kDetailMax + 1];
	std::snprintf(detail, sizeof(detail), "usbd_connect: %s - retrying", strerror(rc));
	model::set_state(LinkState::NoStack, detail);
	return false;
}

// Wait for an event (or a kick / stop) up to ms; qlock held.
void wait_locked(std::unique_lock<std::mutex> &lock, unsigned ms)
{
	qcond.wait_ms(lock, ms, [] { return !queue.empty() || kick || stopping; });
	kick = false;
}

// What is done goes out, on any wake-up. Not sent (a stalled link): kept for
// the next one, the phone's count stays right.
void send_acks()
{
	AckBatch::Counts c;
	{
		std::lock_guard<std::mutex> lock(qlock);
		c = acks.take();
	}
	if (c.total() == 0 || !acc)          // no link: they go nowhere
		return;
	uint8_t payload[proto::kVideoAcksLen];
	c.encode(payload);
	if (!acc->send(proto::kVideoAcks, payload, sizeof(payload))) {
		std::lock_guard<std::mutex> lock(qlock);
		acks.put_back(c);
	}
}

void manager()
{
	unsigned attempt = 0;

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
			wait_locked(lock, kManagerTickMs);
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
		send_acks();
		slog_periodic();
		uint8_t h264_status;
		while (hwvideo::status_pop(&h264_status))
			if (acc)
				acc->send(proto::kH264Status, &h264_status, 1);
		if (!acc)
			acc_reopen_due();
		handshake_due();
		if (switch_since != 0 && log_ms() - switch_since > kSwitchTimeoutMs) {
			switch_since = 0;
			model::set_state(LinkState::Waiting, "phone did not come back as an accessory - replug it");
		}
	}

	log_msg("usb: manager stopping");
	link_drop("stopping", LinkState::Waiting);
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

void wake()
{
	{
		std::lock_guard<std::mutex> lock(qlock);
		kick = true;
	}
	qcond.notify();
}

void ack(PicStatus status)
{
	{
		std::lock_guard<std::mutex> lock(qlock);
		acks.add(status);
		kick = true;
	}
	qcond.notify();
}

void ack_later(PicStatus status)
{
	bool full;
	{
		std::lock_guard<std::mutex> lock(qlock);
		acks.add(status);
		full = acks.full();
		if (full)
			kick = true;
	}
	if (full)
		qcond.notify();
}

bool start(const char *sd_path)
{
	slog_sd = sd_path;
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

} // namespace aoa

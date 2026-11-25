// sbcsrv.cpp - see sbcsrv.h. The resource manager calls are the ones
// common/video/stream_server.cpp uses (QNX 6.3 libc, all exported by the unit's
// libc.so.2).
#include "sbcsrv.h"
#include "sbc.h"
#include "types.h"
#include "wms.h"

extern "C" {
#include "log.h"
}

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

// iofunc.h first (RESMGR_HANDLE_T / RESMGR_OCB_T), as in stream_server.cpp
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <devctl.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr unsigned kNoteReads = 30;
constexpr unsigned kNoteDevctls = 20;
constexpr usize kMaxNotes = 300;
constexpr long kTickNs = 5 * 1000000L;

struct Attr {
	iofunc_attr_t io;
	SbcServer::Impl *impl;
};

struct Pending {
	int rcvid;
	iofunc_ocb_t *ocb;
	u64 off;
	u32 nbytes;
	bool pread;
	unsigned since_ms;
	unsigned seq;
};

u64 mono_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}

} // namespace

struct SbcServer::Impl {
	const std::vector<u8> *data = nullptr;
	const std::vector<u32> *offs = nullptr;
	u32 samples = 0, rate = 0;
	unsigned prebuffer = 0;
	bool partial = true;
	bool loop = false;               // stand-in mode: the clip repeats, no end
	SbcWms wms{-1, 0};
	usize flen = 0;                  // stand-in mode: every frame's length
	std::vector<u8> scratch;         // stand-in mode: a looped reply

	dispatch_t *dpp = nullptr;
	dispatch_context_t *ctp = nullptr;
	int id = -1;
	resmgr_connect_funcs_t connect_funcs;
	resmgr_io_funcs_t io_funcs;
	Attr attr;
	std::thread thread;
	std::atomic<bool> stopping{false};

	std::deque<Pending> pending;     // server thread only
	u64 t0_us = 0;                   // first read; 0 = none yet
	usize out = 0;                   // frames released
	u64 served_end = 0;              // highest byte answered

	std::mutex mu;                   // stats + notes
	SbcStats st;
	std::vector<std::string> notes;

	void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
	{
		char line[200];
		va_list ap;

		va_start(ap, fmt);
		std::vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		std::lock_guard<std::mutex> lock(mu);
		if (notes.size() < kMaxNotes)
			notes.emplace_back(line);
	}

	static constexpr usize kLoopFrames = 0x7fffffff;  // "endless": ~13 days of SBC

	u64 total() const { return loop ? (u64)kLoopFrames * flen : data->size(); }
	bool complete() const { return out >= (loop ? kLoopFrames : offs->size()); }
	u64 avail() const
	{
		if (loop)
			return (u64)out * flen;
		return complete() ? total() : (u64)(*offs)[out];
	}

	void release()
	{
		if (t0_us == 0)
			return;
		out = sbc::released(loop ? kLoopFrames : offs->size(), samples, rate, prebuffer,
		                    mono_us() - t0_us);
		std::lock_guard<std::mutex> lock(mu);
		st.frames_out = (unsigned)out;
	}

	// Answer p if it can be; true if it was.
	bool try_answer(const Pending &p)
	{
		u32 n = 0;
		if (!sbc::answer(p.off, p.nbytes, avail(), total(), complete(), partial, &n))
			return false;
		if (loop && n > 0) {
			scratch.resize(n);
			wms::copy_looped(data->data(), data->size(), p.off, scratch.data(), n);
			MsgReply(p.rcvid, (int)n, scratch.data(), (int)n);
		} else {
			MsgReply(p.rcvid, (int)n, n ? data->data() + p.off : nullptr, (int)n);
		}
		if (!p.pread)
			p.ocb->offset = (off_t)(p.off + n);
		if (p.off + n > served_end)
			served_end = p.off + n;
		unsigned waited = log_ms() - p.since_ms;
		std::lock_guard<std::mutex> lock(mu);
		st.bytes += n;
		if (n == 0)
			st.eof++;
		else if (n < p.nbytes && p.off + p.nbytes < total())
			st.short_reads++;
		if (waited > 5) {
			st.waited++;
			if (waited > st.wait_ms_max)
				st.wait_ms_max = waited;
		}
		if (p.seq <= kNoteReads && notes.size() < kMaxNotes) {
			char line[140];
			std::snprintf(line, sizeof(line), "read #%u answered: %u of %u bytes after %u ms",
			              p.seq, n, p.nbytes, waited);
			notes.emplace_back(line);
		}
		return true;
	}

	void serve_pending()
	{
		for (auto it = pending.begin(); it != pending.end();) {
			if (try_answer(*it))
				it = pending.erase(it);
			else
				++it;
		}
	}

	int on_read(resmgr_context_t *c, io_read_t *msg, iofunc_ocb_t *ocb)
	{
		int status = iofunc_read_verify(c, msg, ocb, nullptr);
		if (status != EOK)
			return status;
		Pending p{};
		switch (msg->i.xtype & _IO_XTYPE_MASK) {
		case _IO_XTYPE_NONE:
			p.off = (u64)ocb->offset;
			break;
		case _IO_XTYPE_OFFSET:
			p.off = (u64)reinterpret_cast<struct _xtype_offset *>(&msg->i + 1)->offset;
			p.pread = true;
			break;
		default:
			return ENOSYS;
		}
		p.rcvid = c->rcvid;
		p.ocb = ocb;
		p.nbytes = (u32)msg->i.nbytes;
		p.since_ms = log_ms();
		if (t0_us == 0) {
			t0_us = mono_us();
			std::lock_guard<std::mutex> lock(mu);
			st.first_read_ms = p.since_ms;
		}
		release();
		{
			std::lock_guard<std::mutex> lock(mu);
			p.seq = ++st.reads;
			if (p.pread)
				st.preads++;
			if (st.min_read == 0 || p.nbytes < st.min_read)
				st.min_read = p.nbytes;
			if (p.nbytes > st.max_read)
				st.max_read = p.nbytes;
		}
		if (p.seq <= kNoteReads)
			note("read #%u: %u bytes at %llu%s, %llu bytes out", p.seq, p.nbytes,
			     (unsigned long long)p.off, p.pread ? " (pread)" : "",
			     (unsigned long long)avail());
		if (!try_answer(p))
			pending.push_back(p);
		return _RESMGR_NOREPLY;
	}

	int on_devctl(resmgr_context_t *c, io_devctl_t *msg, iofunc_ocb_t *ocb)
	{
		unsigned dcmd = (unsigned)msg->i.dcmd, nbytes = (unsigned)msg->i.nbytes;
		unsigned seq;
		{
			std::lock_guard<std::mutex> lock(mu);
			seq = ++st.devctls;
		}
		if (loop) {
			wms::Answer a = wms::devctl(dcmd, nbytes, wms.codec, wms.rate);
			if (a.handled) {
				{
					std::lock_guard<std::mutex> lock(mu);
					st.devctls_answered++;
				}
				note("devctl #%u: dcmd 0x%08x (%s), %u bytes, from pid %d -> answered, %u bytes",
				     seq, dcmd, a.name, nbytes, c->info.pid, a.len);
				std::memset(&msg->o, 0, sizeof(msg->o));
				msg->o.ret_val = 0;
				msg->o.nbytes = (int)a.len;
				std::memcpy(_DEVCTL_DATA(msg->o), a.out, a.len);
				return _RESMGR_PTR(c, &msg->o, sizeof(msg->o) + a.len);
			}
		}
		int rc = iofunc_devctl_default(c, msg, ocb);
		if (seq <= kNoteDevctls || loop)
			note("devctl #%u: dcmd 0x%08x, %u bytes, from pid %d -> %s", seq, dcmd, nbytes,
			     c->info.pid, rc == (int)_RESMGR_DEFAULT ? "not handled (ENOSYS)" : std::strerror(rc));
		return rc;
	}

	int on_unblock(resmgr_context_t *c, io_pulse_t *msg, iofunc_ocb_t *ocb)
	{
		for (auto it = pending.begin(); it != pending.end(); ++it) {
			if (it->rcvid == c->rcvid) {
				note("read #%u unblocked by the client after %u ms", it->seq,
				     log_ms() - it->since_ms);
				MsgError(it->rcvid, EINTR);
				pending.erase(it);
				std::lock_guard<std::mutex> lock(mu);
				st.unblocks++;
				return _RESMGR_NOREPLY;
			}
		}
		return iofunc_unblock_default(c, msg, ocb);
	}

	int on_close(resmgr_context_t *c, void *reserved, iofunc_ocb_t *ocb)
	{
		for (auto it = pending.begin(); it != pending.end();) {
			if (it->ocb == ocb) {
				MsgError(it->rcvid, EBADF);
				it = pending.erase(it);
			} else {
				++it;
			}
		}
		{
			std::lock_guard<std::mutex> lock(mu);
			st.closes++;
		}
		note("closed by pid %d", c->info.pid);
		return iofunc_close_ocb_default(c, reserved, ocb);
	}

	int on_open(resmgr_context_t *c, io_open_t *msg, iofunc_attr_t *a, void *extra)
	{
		int rc = iofunc_open_default(c, msg, a, extra);
		note("open by pid %d, ioflag 0x%x -> %s", c->info.pid, (unsigned)msg->connect.ioflag,
		     rc == EOK ? "ok" : std::strerror(rc));
		if (rc == EOK) {
			std::lock_guard<std::mutex> lock(mu);
			st.opens++;
		}
		return rc;
	}

	void run()
	{
		while (!stopping) {
			dispatch_context_t *c = dispatch_block(ctp);
			if (c != nullptr) {
				ctp = c;
				dispatch_handler(ctp);
			}
			release();
			serve_pending();
		}
		// end of stream for whoever still waits
		if (loop) {
			for (const Pending &p : pending)
				MsgReply(p.rcvid, 0, nullptr, 0);
			pending.clear();
			return;
		}
		out = offs->size();
		serve_pending();
	}
};

namespace {

SbcServer::Impl *impl_of(iofunc_attr_t *a)
{
	return reinterpret_cast<Attr *>(a)->impl;
}

// No exception may pass through libc's resmgr layer (docs/cpp-apps.md).
int io_read(resmgr_context_t *c, io_read_t *msg, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_read(c, msg, ocb);
	} catch (...) {
		return ENOMEM;
	}
}

int io_devctl(resmgr_context_t *c, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_devctl(c, msg, ocb);
	} catch (...) {
		return ENOMEM;
	}
}

int io_unblock(resmgr_context_t *c, io_pulse_t *msg, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_unblock(c, msg, ocb);
	} catch (...) {
		return iofunc_unblock_default(c, msg, ocb);
	}
}

int io_close_ocb(resmgr_context_t *c, void *reserved, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_close(c, reserved, ocb);
	} catch (...) {
		return iofunc_close_ocb_default(c, reserved, ocb);
	}
}

int io_open(resmgr_context_t *c, io_open_t *msg, iofunc_attr_t *a, void *extra)
{
	try {
		return impl_of(a)->on_open(c, msg, a, extra);
	} catch (...) {
		return ENOMEM;
	}
}

} // namespace

SbcServer::~SbcServer()
{
	stop();
}

std::string SbcServer::start(const char *path, const std::vector<u8> *data,
                             const std::vector<u32> *offs, u32 samples, u32 rate,
                             unsigned prebuffer_frames, bool partial, const SbcWms *wms)
{
	if (impl_ != nullptr)
		return "already serving";
	if (data == nullptr || offs == nullptr || offs->empty())
		return "no clip";
	Impl *d = new Impl();
	d->data = data;
	d->offs = offs;
	d->samples = samples;
	d->rate = rate;
	d->prebuffer = prebuffer_frames;
	d->partial = partial;
	d->st.frames = (unsigned)offs->size();
	if (wms != nullptr) {
		// every frame the same length: the looped stream's offsets are k * flen
		d->flen = (offs->size() > 1 ? (*offs)[1] - (*offs)[0] : data->size());
		if (d->flen == 0 || data->size() != d->flen * offs->size()) {
			delete d;
			return "stand-in mode needs frames of one length";
		}
		d->loop = true;
		d->wms = *wms;
	}

	d->dpp = dispatch_create();
	if (d->dpp == nullptr) {
		std::string e = std::string("dispatch_create: ") + std::strerror(errno);
		delete d;
		return e;
	}
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &d->connect_funcs, _RESMGR_IO_NFUNCS, &d->io_funcs);
	d->connect_funcs.open = io_open;
	d->io_funcs.read = io_read;
	d->io_funcs.devctl = io_devctl;
	d->io_funcs.unblock = io_unblock;
	d->io_funcs.close_ocb = io_close_ocb;
	// stand-in: the stack's node is mode 050666 (S_IFNAM), size 0
	iofunc_attr_init(&d->attr.io, d->loop ? (S_IFNAM | 0666) : (S_IFREG | 0444), nullptr, nullptr);
	d->attr.io.nbytes = d->loop ? 0 : (off_t)data->size();
	d->attr.impl = d;

	resmgr_attr_t ra;
	std::memset(&ra, 0, sizeof(ra));
	ra.nparts_max = 1;
	ra.msg_max_size = 2048;
	d->id = resmgr_attach(d->dpp, &ra, path, _FTYPE_ANY, 0, &d->connect_funcs, &d->io_funcs,
	                      &d->attr.io);
	if (d->id == -1) {
		std::string e = std::string("resmgr_attach ") + path + ": " + std::strerror(errno);
		dispatch_destroy(d->dpp);
		delete d;
		return e;
	}
	struct timespec tick = {0, kTickNs};
	dispatch_timeout(d->dpp, &tick);
	d->ctp = dispatch_context_alloc(d->dpp);
	if (d->ctp == nullptr) {
		std::string e = std::string("dispatch_context_alloc: ") + std::strerror(errno);
		resmgr_detach(d->dpp, d->id, 0);
		dispatch_destroy(d->dpp);
		delete d;
		return e;
	}
	impl_ = d;
	d->thread = std::thread([d] { d->run(); });
	return "";
}

void SbcServer::stop()
{
	Impl *d = impl_;
	if (d == nullptr)
		return;
	d->stopping = true;
	if (d->thread.joinable())
		d->thread.join();
	resmgr_detach(d->dpp, d->id, _RESMGR_DETACH_ALL);
	dispatch_context_free(d->ctp);
	dispatch_destroy(d->dpp);
	impl_ = nullptr;
	delete d;
}

SbcStats SbcServer::stats()
{
	if (impl_ == nullptr)
		return SbcStats();
	std::lock_guard<std::mutex> lock(impl_->mu);
	return impl_->st;
}

std::vector<std::string> SbcServer::take_notes()
{
	std::vector<std::string> out;
	if (impl_ == nullptr)
		return out;
	std::lock_guard<std::mutex> lock(impl_->mu);
	out.swap(impl_->notes);
	return out;
}

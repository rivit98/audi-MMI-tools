// stream_server.cpp - see stream_server.h. Moved from hello9, where it
// served a file on a timed schedule.
//
// Resource manager calls as in QNX 6.3's libc (all exported by the unit's
// libc.so.2). <sys/iomsg.h> needs <sys/pm.h>, which the sysroot lacks:
// src/common/qnx/sys/pm.h stands in.
#include "stream_server.h"

extern "C" {
#include "log.h"
}

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

// iofunc.h first: it makes the resmgr callbacks take iofunc_attr_t /
// iofunc_ocb_t (RESMGR_HANDLE_T, RESMGR_OCB_T) before resmgr.h defaults
// them to void.
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr unsigned kNoteReads = 40;     // reads noted in detail
// A read starting this far past the live edge is answered at once (with
// filler frames), not held: a probe of the file's end (ID3v1, last box)
// would otherwise wait for the declared last frame, hours away, with the
// player's attach call stuck behind it. Counted as far reads.
constexpr unsigned kFarS = 10;
constexpr size_t kMaxNotes = 400;
constexpr long kTickNs = 10 * 1000000L; // dispatch timeout: serve the queue

// Our attribute: the iofunc one first, so the library's pointer is ours.
struct Attr {
	iofunc_attr_t io;
	StreamServer::Impl *impl;
};

struct Pending {
	int rcvid;
	iofunc_ocb_t *ocb;
	uint64_t off;
	unsigned nbytes;
	bool pread;
	unsigned since_ms;
	unsigned seq;
};

} // namespace

struct StreamServer::Impl {
	live::Stream *stream = nullptr;
	uint64_t data_off = 0, total = 0;
	uint32_t sample_size = 0;
	unsigned fps = 25;

	dispatch_t *dpp = nullptr;
	dispatch_context_t *ctp = nullptr;
	int id = -1;
	resmgr_connect_funcs_t connect_funcs;
	resmgr_io_funcs_t io_funcs;
	Attr attr;

	std::atomic<bool> stopping{false};

	std::deque<Pending> pending;             // server thread only
	std::vector<uint8_t> buf;

	std::mutex mu;                           // stats + notes + history
	StreamStats st;
	std::vector<std::string> notes;
	ReadRecord history[StreamServer::kReadHistory];   // by seq % kReadHistory

	uint64_t frame_of(uint64_t off) const { return (off - data_off) / sample_size; }

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

	// Does the read start more than kFarS past the live edge?
	bool far(const Pending &p) const
	{
		if (p.off < data_off || p.off >= total || stream->finished())
			return false;
		return frame_of(p.off) > stream->frames() + (uint64_t)kFarS * fps;
	}

	// Answer one read with what is available, n bytes at most; 0 = EOF. A
	// far read gets its bytes regardless (see kFarMs).
	void reply(const Pending &p)
	{
		uint64_t avail = far(p) ? total : stream->available(), n = p.nbytes;

		if (p.off >= avail)
			n = 0;
		else if (p.off + n > avail)
			n = avail - p.off;
		buf.resize(n);
		live::Stream::ReadInfo info;
		n = stream->read(p.off, buf.data(), (size_t)n, &info);
		MsgReply(p.rcvid, (int)n, buf.data(), (int)n);
		if (!p.pread)
			p.ocb->offset = (off_t)(p.off + n);

		unsigned waited = log_ms() - p.since_ms;
		std::lock_guard<std::mutex> lock(mu);
		ReadRecord &r = history[p.seq % StreamServer::kReadHistory];
		if (r.seq == p.seq) {
			r.answered = true;
			r.served = (unsigned)n;
			r.waited_ms = waited;
			r.evicted = info.evicted;
			r.ahead = info.ahead;
		}
		st.bytes += n;
		if (n == 0)
			st.eof++;
		if (p.off + n > st.served_end)
			st.served_end = p.off + n;
		if (waited > 5) {
			st.waited++;
			st.wait_ms_sum += waited;
			if (waited > st.wait_ms_max)
				st.wait_ms_max = waited;
		}
		if (p.seq <= kNoteReads && waited > 5 && notes.size() < kMaxNotes) {
			char line[120];
			std::snprintf(line, sizeof(line), "read #%u answered after %u ms: %llu bytes",
			              p.seq, waited, (unsigned long long)n);
			notes.emplace_back(line);
		}
	}

	// The whole range is there (clamped at the declared end), or it never
	// will be.
	bool ready(const Pending &p) const
	{
		uint64_t end = p.off + p.nbytes;
		if (end > total)
			end = total;
		return stream->finished() || p.off >= total || end <= stream->available() || far(p);
	}

	// Each queued read on its own: another fd's read may be ready while an
	// earlier one still waits.
	void serve_pending()
	{
		for (auto it = pending.begin(); it != pending.end();) {
			if (ready(*it)) {
				Pending p = *it;
				it = pending.erase(it);
				reply(p);
			} else {
				++it;
			}
		}
		std::lock_guard<std::mutex> lock(mu);
		st.waiting_now = (unsigned)pending.size();
	}

	int on_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
	{
		int status = iofunc_read_verify(ctp, msg, ocb, nullptr);
		if (status != EOK)
			return status;
		Pending p{};
		switch (msg->i.xtype & _IO_XTYPE_MASK) {
		case _IO_XTYPE_NONE:
			p.off = (uint64_t)ocb->offset;
			break;
		case _IO_XTYPE_OFFSET:
			p.off = (uint64_t)reinterpret_cast<struct _xtype_offset *>(&msg->i + 1)->offset;
			p.pread = true;
			break;
		default:
			return ENOSYS;
		}
		p.rcvid = ctp->rcvid;
		p.ocb = ocb;
		p.nbytes = (unsigned)msg->i.nbytes;
		p.since_ms = log_ms();

		uint64_t end = p.off + p.nbytes;
		if (end > total)
			end = total;
		// frames [0, needed) must exist for the whole read; lead = how many
		// past what exists now
		uint64_t needed = end <= data_off ? 0 : (end - data_off + sample_size - 1) / sample_size;
		int64_t lead = (int64_t)needed - (int64_t)stream->frames();
		{
			std::lock_guard<std::mutex> lock(mu);
			p.seq = ++st.reads;
			if (p.pread)
				st.preads++;
			if (st.min_read == 0 || p.nbytes < st.min_read)
				st.min_read = p.nbytes;
			if (p.nbytes > st.max_read)
				st.max_read = p.nbytes;
			if (needed > 0) {
				st.lead_frames_last = lead;
				if (lead > st.lead_frames_max)
					st.lead_frames_max = lead;
				if (lead > st.lead_win_max)
					st.lead_win_max = lead;
				if (lead < st.lead_win_min)
					st.lead_win_min = lead;
			}
			ReadRecord &r = history[p.seq % StreamServer::kReadHistory];
			r = ReadRecord();
			r.seq = p.seq;
			r.at_ms = p.since_ms;
			r.off = p.off;
			r.nbytes = p.nbytes;
			r.first = p.off < data_off ? -1 : (int64_t)frame_of(p.off);
			r.last = needed ? (int64_t)needed - 1 : -1;
			r.lead = lead;
		}
		if (p.seq <= kNoteReads)
			note("read #%u: %u bytes at %llu%s (frame %lld..%lld)%s, lead %lld frames", p.seq,
			     p.nbytes, (unsigned long long)p.off, p.pread ? " (pread)" : "",
			     p.off < data_off ? -1LL : (long long)frame_of(p.off),
			     needed ? (long long)needed - 1 : -1LL,
			     ready(p) ? "" : ", waits", (long long)lead);
		if (far(p)) {
			note("read #%u is a far read (%lld frames past the live edge): answered at once",
			     p.seq, (long long)lead);
			std::lock_guard<std::mutex> lock(mu);
			st.far++;
			history[p.seq % StreamServer::kReadHistory].far = true;
		}
		if (ready(p))
			reply(p);
		else
			pending.push_back(p);
		return _RESMGR_NOREPLY;
	}

	// The client gave up waiting (signal, timeout): answer EINTR.
	int on_unblock(resmgr_context_t *ctp, io_pulse_t *msg, iofunc_ocb_t *ocb)
	{
		for (auto it = pending.begin(); it != pending.end(); ++it) {
			if (it->rcvid == ctp->rcvid) {
				note("read #%u unblocked by the client after %u ms", it->seq,
				     log_ms() - it->since_ms);
				MsgError(it->rcvid, EINTR);
				pending.erase(it);
				std::lock_guard<std::mutex> lock(mu);
				st.unblocked++;
				return _RESMGR_NOREPLY;
			}
		}
		return iofunc_unblock_default(ctp, msg, ocb);
	}

	int on_close(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *ocb)
	{
		for (auto it = pending.begin(); it != pending.end();) {
			if (it->ocb == ocb) {
				MsgError(it->rcvid, EBADF);
				it = pending.erase(it);
			} else {
				++it;
			}
		}
		note("closed by the client");
		return iofunc_close_ocb_default(ctp, reserved, ocb);
	}

	int on_open(resmgr_context_t *ctp, io_open_t *msg, iofunc_attr_t *a, void *extra)
	{
		int rc = iofunc_open_default(ctp, msg, a, extra);
		if (rc == EOK) {
			note("opened by pid %d", ctp->info.pid);
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
			stream->fill(log_ms());
			serve_pending();
		}
	}
};

namespace {

StreamServer::Impl *impl_of(iofunc_attr_t *a)
{
	return reinterpret_cast<Attr *>(a)->impl;
}

// The callbacks are called from libc's resmgr layer: an exception must not
// pass through it (no unwind tables, docs/cpp-apps.md), so each catches all.
int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_read(ctp, msg, ocb);
	} catch (...) {
		return ENOMEM;
	}
}

int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_unblock(ctp, msg, ocb);
	} catch (...) {
		return iofunc_unblock_default(ctp, msg, ocb);
	}
}

int io_close_ocb(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *ocb)
{
	try {
		return impl_of(ocb->attr)->on_close(ctp, reserved, ocb);
	} catch (...) {
		return iofunc_close_ocb_default(ctp, reserved, ocb);
	}
}

int io_open(resmgr_context_t *ctp, io_open_t *msg, iofunc_attr_t *a, void *extra)
{
	try {
		return impl_of(a)->on_open(ctp, msg, a, extra);
	} catch (...) {
		return ENOMEM;
	}
}

} // namespace

StreamServer::~StreamServer()
{
	stop();
}

std::string StreamServer::start(const char *path, live::Stream *stream)
{
	if (impl_ != nullptr)
		return "already serving";
	if (stream == nullptr || stream->declared() == 0)
		return "no stream configured";
	Impl *d = new Impl();
	d->stream = stream;
	d->data_off = stream->data_off();
	d->total = stream->total();
	d->sample_size = stream->sample_size();
	d->fps = stream->config().fps;

	d->dpp = dispatch_create();
	if (d->dpp == nullptr) {
		std::string e = std::string("dispatch_create: ") + std::strerror(errno);
		delete d;
		return e;
	}
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &d->connect_funcs, _RESMGR_IO_NFUNCS, &d->io_funcs);
	d->connect_funcs.open = io_open;
	d->io_funcs.read = io_read;
	d->io_funcs.unblock = io_unblock;
	d->io_funcs.close_ocb = io_close_ocb;
	iofunc_attr_init(&d->attr.io, S_IFREG | 0444, nullptr, nullptr);
	d->attr.io.nbytes = (off_t)d->total;
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
	last_ = StreamStats();
	impl_ = d;
	thread_ = std::thread([d] { d->run(); });
	return "";
}

void StreamServer::stop()
{
	if (impl_ == nullptr)
		return;
	impl_->stream->finish();
	impl_->stopping = true;
	if (thread_.joinable())
		thread_.join();
	// any read still queued gets EOF; then the path goes away
	for (const Pending &p : impl_->pending)
		MsgReply(p.rcvid, 0, nullptr, 0);
	impl_->pending.clear();
	resmgr_detach(impl_->dpp, impl_->id, _RESMGR_DETACH_ALL);
	dispatch_context_free(impl_->ctp);
	dispatch_destroy(impl_->dpp);
	last_ = impl_->st;
	delete impl_;
	impl_ = nullptr;
}

StreamStats StreamServer::stats()
{
	if (impl_ == nullptr)
		return last_;
	std::lock_guard<std::mutex> lock(impl_->mu);
	return impl_->st;
}

StreamStats StreamServer::stats_window()
{
	if (impl_ == nullptr)
		return last_;
	std::lock_guard<std::mutex> lock(impl_->mu);
	StreamStats s = impl_->st;
	impl_->st.lead_win_min = INT64_MAX;
	impl_->st.lead_win_max = INT64_MIN;
	return s;
}

std::vector<ReadRecord> StreamServer::recent_reads()
{
	std::vector<ReadRecord> out;
	if (impl_ == nullptr)
		return out;
	std::lock_guard<std::mutex> lock(impl_->mu);
	unsigned last = impl_->st.reads;
	unsigned n = last < kReadHistory ? last : kReadHistory;
	for (unsigned seq = last - n + 1; n > 0 && seq <= last; seq++) {
		const ReadRecord &r = impl_->history[seq % kReadHistory];
		if (r.seq == seq)
			out.push_back(r);
	}
	return out;
}

std::vector<std::string> StreamServer::take_notes()
{
	std::vector<std::string> out;
	if (impl_ == nullptr)
		return out;
	std::lock_guard<std::mutex> lock(impl_->mu);
	out.swap(impl_->notes);
	return out;
}

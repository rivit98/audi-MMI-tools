// perf.cpp - picture timing records, summarised and written off the hot
// path (see perf.h).
#include "perf.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include "cpumon.h"
#include "log.h"
}

#include "jpeg_pacer.h"
#include "mono_cond.h"
#include "rate_limit.h"

namespace perf {

namespace {

constexpr unsigned kRing = 512;
constexpr unsigned kCsvLineMax = 200;
constexpr unsigned kWarnEveryMs = 10000;

// new columns go at the end: tools/plot_stream_perf.py reads older files too
constexpr const char *kCsvHeader =
	"t_ms,id,status,w,h,bytes,rx_us,wait_us,decode_us,render_us,"
	"copy_us,flip_us,vfb_bytes,vfb_rows,vfb_full,latency_us,phone_ms,cap_us,hold_us\n";

// --- shared (mu) --------------------------------------------------------
std::mutex mu;
MonoCond   cond;
Pic        ring[kRing];
unsigned   head, count, lost;
unsigned   idle_us;          // since the writer last took it
bool       flush_req, stop_req;

std::atomic<bool> started{false};
std::thread       writer_thread;

// --- writer thread only -------------------------------------------------
FILE    *csv;
char     csv_buf[kBatch * kCsvLineMax];
unsigned csv_len, csv_lines, csv_at;
bool     csv_dead;

// avg/max of one stage
struct Stat {
	unsigned long long sum = 0;
	unsigned max = 0;

	void add(unsigned v)
	{
		sum += v;
		if (v > max)
			max = v;
	}

	// "avg/max" in ms; n samples
	void fmt(char *out, size_t cap, unsigned n) const
	{
		std::snprintf(out, cap, "%u/%u", n ? (unsigned)(sum / n / 1000) : 0, max / 1000);
	}
};

long long cpu_us()
{
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return -1;
	return (long long)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
	       ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

struct Agg {
	unsigned start_ms = 0, shown = 0, bad = 0, dropped = 0, full = 0;
	unsigned capped = 0, held = 0;    // shown pictures the cap held back / whose ack it held
	unsigned long long vfb_bytes = 0, vfb_rows = 0, jpeg_bytes = 0;
	Stat rx, wait, decode, render, copy, flip, latency, phone, cap, hold;
	unsigned long long idle_us = 0;
	long long cpu_us0 = 0;

	void reset(unsigned now)
	{
		*this = Agg();
		start_ms = now;
		cpu_us0 = cpu_us();
	}

	void add(const Pic &p)
	{
		if (p.status == proto::PicStatus::Bad) {
			bad++;
			return;
		}
		if (p.status == proto::PicStatus::Dropped) {
			dropped++;
			return;
		}
		shown++;
		jpeg_bytes += p.bytes;
		phone.add(p.phone_ms * 1000);
		vfb_bytes += p.vfb_bytes;
		vfb_rows += p.vfb_rows;
		full += p.vfb_full;
		rx.add(p.rx_us);
		wait.add(p.wait_us);
		decode.add(p.decode_us);
		render.add(p.render_us);
		copy.add(p.copy_us);
		flip.add(p.flip_us);
		latency.add(p.latency_us);
		if (p.cap_us >= 1000) {
			capped++;
			cap.add(p.cap_us);
		}
		if (p.hold_us >= 1000) {
			held++;
			hold.add(p.hold_us);
		}
	}

	// main loop busy % over the span
	unsigned busy(unsigned now) const
	{
		unsigned span = now - start_ms;
		unsigned b = span ? 100 - (unsigned)(idle_us / 10 / span) : 0;
		return b > 100 ? 0 : b;
	}

	void log(const char *what, unsigned now) const
	{
		char s_rx[24], s_wait[24], s_dec[24], s_ren[24], s_cpy[24], s_flp[24], s_lat[24];
		char s_cpu[16], s_fmt[160], s_ph[24], s_cap[24], s_hold[24];
		unsigned span = now - start_ms, n = shown;
		long long c = cpu_us();

		if (span == 0)
			span = 1;
		rx.fmt(s_rx, sizeof(s_rx), n);
		wait.fmt(s_wait, sizeof(s_wait), n);
		decode.fmt(s_dec, sizeof(s_dec), n);
		render.fmt(s_ren, sizeof(s_ren), n);
		copy.fmt(s_cpy, sizeof(s_cpy), n);
		flip.fmt(s_flp, sizeof(s_flp), n);
		latency.fmt(s_lat, sizeof(s_lat), n);
		cap.fmt(s_cap, sizeof(s_cap), capped);
		hold.fmt(s_hold, sizeof(s_hold), held);
		if (n) {
			phone.fmt(s_ph, sizeof(s_ph), n);
			std::snprintf(s_fmt, sizeof(s_fmt), "JPEG %u x %u KB, decode %s, phone %s",
			              n, (unsigned)(jpeg_bytes / n / 1024), s_dec, s_ph);
		} else {
			std::snprintf(s_fmt, sizeof(s_fmt), "no pictures");
		}
		if (c >= 0 && cpu_us0 >= 0)
			std::snprintf(s_cpu, sizeof(s_cpu), "%u%%", (unsigned)((c - cpu_us0) / 10 / span));
		else
			std::snprintf(s_cpu, sizeof(s_cpu), "?");
		log_msg("perf: %s %u.%u s: %u shown %u.%u fps, %u dropped, %u bad | ms avg/max: "
		        "rx %s wait %s decode %s render %s copy %s flip %s, latency %s | "
		        "%s | vfb %u KB %u rows, %u full | main busy %u%%, cpu %s | "
		        "cap %u fps: %u delayed %s, %u acks held %s",
		        what, span / 1000, span / 100 % 10, n, n * 10000 / span / 10,
		        n * 10000 / span % 10, dropped, bad, s_rx, s_wait, s_dec, s_ren, s_cpy, s_flp, s_lat,
		        s_fmt,
		        n ? (unsigned)(vfb_bytes / n / 1024) : 0,
		        n ? (unsigned)(vfb_rows / n) : 0, full, busy(now), s_cpu,
		        JpegPacer::kMaxJpegFps, capped, s_cap, held, s_hold);
	}
};

Agg win, total;
bool in_stream;
RateLimit warn_busy(kWarnEveryMs), warn_cap(kWarnEveryMs);

// After a window's line: what in it needs a look.
void warn_window(const Agg &w, unsigned now)
{
	unsigned span = now - w.start_ms, busy = w.busy(now), held;

	if (w.shown == 0)
		return;
	if (busy >= kHighBusyPct && warn_busy.allow(now, &held))
		log_msg("perf: HIGH main load: main loop %u%% busy over %u.%u s of JPEG "
		        "(%u more not logged)", busy, span / 1000, span / 100 % 10, held);
	// every picture of a window (1+ fps) waited for the cap
	if (w.capped == w.shown && w.shown * 1000 >= span && warn_cap.allow(now, &held))
		log_msg("perf: JPEG cap hit for the whole window: the phone sends faster than "
		        "%u fps (%u more not logged)", JpegPacer::kMaxJpegFps, held);
}

void csv_write(unsigned now)
{
	if (csv_len == 0)
		return;
	if (csv != nullptr && !csv_dead) {
		if (fwrite(csv_buf, 1, csv_len, csv) != csv_len || fflush(csv) != 0 ||
		    fsync(fileno(csv)) != 0) {
			csv_dead = true;           // card gone: stop trying
			log_msg("perf: audi_stream_perf.csv write failed: %s", strerror(errno));
		}
	}
	csv_len = 0;
	csv_lines = 0;
	csv_at = now;
}

void csv_add(const Pic &p)
{
	if (csv_len + kCsvLineMax > sizeof(csv_buf))
		csv_write(log_ms());
	int n = std::snprintf(csv_buf + csv_len, kCsvLineMax,
	                      "%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u\n",
	                      p.t_ms, p.id, (int)p.status, p.w, p.h, p.bytes, p.rx_us,
	                      p.wait_us, p.decode_us, p.render_us, p.copy_us, p.flip_us,
	                      p.vfb_bytes, p.vfb_rows, (int)p.vfb_full, p.latency_us,
	                      p.phone_ms, p.cap_us, p.hold_us);
	if (n > 0 && (unsigned)n < kCsvLineMax) {
		csv_len += (unsigned)n;
		csv_lines++;
	}
}

void lower_priority()
{
	struct sched_param sp;
	int policy;
	int rc = pthread_getschedparam(pthread_self(), &policy, &sp);

	if (rc == 0 && sp.sched_priority > 1) {
		sp.sched_priority--;
		rc = pthread_setschedparam(pthread_self(), policy, &sp);
	}
	log_msg("perf: writer thread priority %d (%s)", sp.sched_priority,
	        rc ? strerror(rc) : "ok");
}

void writer()
{
	static Pic batch[kRing];             // not on the thread's stack

	lower_priority();
	cpumon_name_thread("perf");
	for (;;) {
		unsigned n = 0, idle, dropped;
		bool do_flush, stop;
		{
			std::unique_lock<std::mutex> lock(mu);
			if (count == 0 && !flush_req && !stop_req)
				cond.wait_ms(lock, 1000, [] { return count > 0 || flush_req || stop_req; });
			for (; count > 0; n++, count--, head = (head + 1) % kRing)
				batch[n] = ring[head];
			idle = idle_us;
			idle_us = 0;
			dropped = lost;
			lost = 0;
			stop = stop_req;
			do_flush = flush_req || stop;
			flush_req = false;
		}

		unsigned now = log_ms();
		if (dropped)
			log_msg("perf: %u records lost (writer too slow)", dropped);
		if (n > 0 && !in_stream) {
			in_stream = true;
			win.reset(now);
			total.reset(now);
		}
		if (in_stream) {
			win.idle_us += idle;
			total.idle_us += idle;
		}
		for (unsigned i = 0; i < n; i++) {
			win.add(batch[i]);
			total.add(batch[i]);
			csv_add(batch[i]);
		}
		if (in_stream && (do_flush || now - win.start_ms >= kWindowMs)) {
			if (win.shown + win.dropped + win.bad > 0) {
				win.log("window", now);
				warn_window(win, now);
			}
			win.reset(now);
		}
		if (do_flush && in_stream) {
			total.log("stream", now);
			in_stream = false;
		}
		if (do_flush || csv_lines >= kBatch || (csv_lines > 0 && now - csv_at >= kBatchMs))
			csv_write(now);
		if (stop)
			return;
	}
}

} // namespace

void init(const char *out_dir)
{
	struct timespec res;
	char path[256];

	if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
		log_msg("perf: clock resolution %ld us", res.tv_nsec / 1000 + res.tv_sec * 1000000);
	std::snprintf(path, sizeof(path), "%s/audi_stream_perf.csv", out_dir);
	csv = fopen(path, "w");
	if (csv == nullptr) {
		log_msg("perf: cannot open %s: %s (summaries only)", path, strerror(errno));
	} else {
		fputs(kCsvHeader, csv);
		fflush(csv);
	}
	csv_at = log_ms();

	try {
		writer_thread = std::thread(writer);
		started = true;
	} catch (const std::system_error &e) {
		log_msg("perf: writer thread: %s (no perf records)", e.what());
	}
}

void shutdown()
{
	if (!started)
		return;
	started = false;
	{
		std::lock_guard<std::mutex> lock(mu);
		stop_req = true;
	}
	cond.notify();
	writer_thread.join();
	if (csv != nullptr)
		fclose(csv);
	csv = nullptr;
}

void add(const Pic &p)
{
	if (!started)
		return;
	{
		std::lock_guard<std::mutex> lock(mu);
		if (count == kRing) {
			lost++;
		} else {
			ring[(head + count) % kRing] = p;
			count++;
		}
	}
	cond.notify();
}

void idle(unsigned us)
{
	std::lock_guard<std::mutex> lock(mu);
	idle_us += us;
}

void flush()
{
	if (!started)
		return;
	{
		std::lock_guard<std::mutex> lock(mu);
		flush_req = true;
	}
	cond.notify();
}

} // namespace perf

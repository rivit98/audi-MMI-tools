/*
 * perf.c - picture timing records, summarised and written off the hot
 * path (see perf.h).
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "cpumon.h"
#include "log.h"
#include "perf.h"
#include "proto.h"

#define RING     512
#define CSV_LINE_MAX 176

#define CSV_HEADER "t_ms,id,status,w,h,bytes,rx_us,wait_us,decode_us,render_us," \
                   "copy_us,flip_us,vfb_bytes,vfb_rows,vfb_full,latency_us,phone_ms\n"

/* --- shared (lock) ------------------------------------------------------ */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond;
static struct perf_pic ring[RING];
static unsigned        head, count, lost;
static unsigned        idle_us;          /* since the writer last took it */
static int             flush_req, started;

/* --- writer thread only ---------------------------------------------------- */
static FILE    *csv;
static char     csv_buf[PERF_BATCH * CSV_LINE_MAX];
static unsigned csv_len, csv_lines, csv_at;
static int      csv_dead;

/* avg/max of one stage */
struct stat1 {
	unsigned long long sum;
	unsigned max;
};

/* the JPEG pictures */
struct fmt1 {
	unsigned n;
	unsigned long long bytes;
	struct stat1 decode, phone;
};

struct agg {
	unsigned start_ms, shown, bad, dropped, full;
	unsigned long long vfb_bytes, vfb_rows;
	struct stat1 rx, wait, decode, render, copy, flip, latency;
	struct fmt1 fmt;
	unsigned long long idle_us;
	long long cpu_us0;
};

static struct agg win, total;
static int in_stream;

static void st_add(struct stat1 *s, unsigned v)
{
	s->sum += v;
	if (v > s->max)
		s->max = v;
}

/* "avg/max" in ms; n samples */
static int st_fmt(char *out, size_t cap, const struct stat1 *s, unsigned n)
{
	return snprintf(out, cap, "%u/%u", n ? (unsigned)(s->sum / n / 1000) : 0,
	                s->max / 1000);
}

static long long cpu_us(void)
{
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return -1;
	return (long long)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
	       ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

static void agg_reset(struct agg *a, unsigned now)
{
	memset(a, 0, sizeof(*a));
	a->start_ms = now;
	a->cpu_us0 = cpu_us();
}

static void agg_add(struct agg *a, const struct perf_pic *p)
{
	struct fmt1 *f;

	if (p->status == PROTO_VIDEO_BAD) {
		a->bad++;
		return;
	}
	if (p->status == PROTO_VIDEO_DROPPED) {
		a->dropped++;
		return;
	}
	f = &a->fmt;
	f->n++;
	f->bytes += p->bytes;
	st_add(&f->decode, p->decode_us);
	st_add(&f->phone, p->phone_ms * 1000);
	a->shown++;
	a->vfb_bytes += p->vfb_bytes;
	a->vfb_rows += p->vfb_rows;
	a->full += p->vfb_full != 0;
	st_add(&a->rx, p->rx_us);
	st_add(&a->wait, p->wait_us);
	st_add(&a->decode, p->decode_us);
	st_add(&a->render, p->render_us);
	st_add(&a->copy, p->copy_us);
	st_add(&a->flip, p->flip_us);
	st_add(&a->latency, p->latency_us);
}

static void agg_log(const char *what, const struct agg *a, unsigned now)
{
	char rx[24], wait[24], dec[24], ren[24], cpy[24], flp[24], lat[24], cpu[16];
	char fmts[160], d[24], ph[24];
	unsigned span = now - a->start_ms, n = a->shown;
	unsigned busy = span ? 100 - (unsigned)(a->idle_us / 10 / span) : 0;
	long long c = cpu_us();

	if (span == 0)
		span = 1;
	st_fmt(rx, sizeof(rx), &a->rx, n);
	st_fmt(wait, sizeof(wait), &a->wait, n);
	st_fmt(dec, sizeof(dec), &a->decode, n);
	st_fmt(ren, sizeof(ren), &a->render, n);
	st_fmt(cpy, sizeof(cpy), &a->copy, n);
	st_fmt(flp, sizeof(flp), &a->flip, n);
	st_fmt(lat, sizeof(lat), &a->latency, n);
	if (a->fmt.n) {
		const struct fmt1 *f = &a->fmt;

		st_fmt(d, sizeof(d), &f->decode, f->n);
		st_fmt(ph, sizeof(ph), &f->phone, f->n);
		snprintf(fmts, sizeof(fmts), "JPEG %u x %u KB, decode %s, phone %s",
		         f->n, (unsigned)(f->bytes / f->n / 1024), d, ph);
	} else {
		snprintf(fmts, sizeof(fmts), "no pictures");
	}
	if (c >= 0 && a->cpu_us0 >= 0)
		snprintf(cpu, sizeof(cpu), "%u%%", (unsigned)((c - a->cpu_us0) / 10 / span));
	else
		snprintf(cpu, sizeof(cpu), "?");
	log_msg("perf: %s %u.%u s: %u shown %u.%u fps, %u dropped, %u bad | ms avg/max: "
	        "rx %s wait %s decode %s render %s copy %s flip %s, latency %s | "
	        "%s | vfb %u KB %u rows, %u full | main busy %u%%, cpu %s",
	        what, span / 1000, span / 100 % 10, n, n * 10000 / span / 10,
	        n * 10000 / span % 10, a->dropped, a->bad, rx, wait, dec, ren, cpy, flp, lat,
	        fmts,
	        n ? (unsigned)(a->vfb_bytes / n / 1024) : 0,
	        n ? (unsigned)(a->vfb_rows / n) : 0, a->full, busy > 100 ? 0 : busy, cpu);
}

static void csv_write(unsigned now)
{
	if (csv_len == 0)
		return;
	if (csv != NULL && !csv_dead) {
		if (fwrite(csv_buf, 1, csv_len, csv) != csv_len || fflush(csv) != 0 ||
		    fsync(fileno(csv)) != 0) {
			csv_dead = 1;              /* card gone: stop trying */
			log_msg("perf: audi_stream_perf.csv write failed: %s", strerror(errno));
		}
	}
	csv_len = 0;
	csv_lines = 0;
	csv_at = now;
}

static void csv_add(const struct perf_pic *p)
{
	int n;

	if (csv_len + CSV_LINE_MAX > sizeof(csv_buf))
		csv_write(log_ms());
	n = snprintf(csv_buf + csv_len, CSV_LINE_MAX,
	             "%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u\n",
	             p->t_ms, p->id, p->status, p->w, p->h, p->bytes, p->rx_us,
	             p->wait_us, p->decode_us, p->render_us, p->copy_us, p->flip_us,
	             p->vfb_bytes, p->vfb_rows, p->vfb_full, p->latency_us,
	             p->phone_ms);
	if (n > 0 && n < CSV_LINE_MAX) {
		csv_len += (unsigned)n;
		csv_lines++;
	}
}

static void lower_priority(void)
{
	struct sched_param sp;
	int policy, rc;

	rc = pthread_getschedparam(pthread_self(), &policy, &sp);
	if (rc == 0 && sp.sched_priority > 1) {
		sp.sched_priority--;
		rc = pthread_setschedparam(pthread_self(), policy, &sp);
	}
	log_msg("perf: writer thread priority %d (%s)", sp.sched_priority,
	        rc ? strerror(rc) : "ok");
}

static void *writer(void *arg)
{
	static struct perf_pic batch[RING];      /* not on the thread's stack */
	struct timespec ts;
	unsigned n, i, idle, dropped, now;
	int flush;

	(void)arg;
	lower_priority();
	cpumon_name_thread("perf");
	for (;;) {
		pthread_mutex_lock(&lock);
		if (count == 0 && !flush_req) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&cond, &lock, &ts);
		}
		for (n = 0; count > 0; n++, count--, head = (head + 1) % RING)
			batch[n] = ring[head];
		idle = idle_us;
		idle_us = 0;
		dropped = lost;
		lost = 0;
		flush = flush_req;
		flush_req = 0;
		pthread_mutex_unlock(&lock);

		now = log_ms();
		if (dropped)
			log_msg("perf: %u records lost (writer too slow)", dropped);
		if (n > 0 && !in_stream) {
			in_stream = 1;
			agg_reset(&win, now);
			agg_reset(&total, now);
		}
		if (in_stream) {
			win.idle_us += idle;
			total.idle_us += idle;
		}
		for (i = 0; i < n; i++) {
			agg_add(&win, &batch[i]);
			agg_add(&total, &batch[i]);
			csv_add(&batch[i]);
		}
		if (in_stream && (flush || now - win.start_ms >= PERF_WINDOW_MS)) {
			if (win.shown + win.dropped + win.bad > 0)
				agg_log("window", &win, now);
			agg_reset(&win, now);
		}
		if (flush && in_stream) {
			agg_log("stream", &total, now);
			in_stream = 0;
		}
		if (flush || csv_lines >= PERF_BATCH ||
		    (csv_lines > 0 && now - csv_at >= PERF_BATCH_MS))
			csv_write(now);
	}
	return NULL;
}

void perf_init(const char *sd_path)
{
	pthread_condattr_t ca;
	pthread_t tid;
	struct timespec res;
	char path[256];
	int rc;

	if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
		log_msg("perf: clock resolution %ld us", res.tv_nsec / 1000 + res.tv_sec * 1000000);
	snprintf(path, sizeof(path), "%s/audi_stream_perf.csv", sd_path);
	csv = fopen(path, "w");
	if (csv == NULL) {
		log_msg("perf: cannot open %s: %s (summaries only)", path, strerror(errno));
	} else {
		fputs(CSV_HEADER, csv);
		fflush(csv);
	}
	csv_at = log_ms();

	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&cond, &ca);
	pthread_condattr_destroy(&ca);
	rc = pthread_create(&tid, NULL, writer, NULL);
	if (rc != 0) {
		log_msg("perf: writer thread: %s (no perf records)", strerror(rc));
		return;
	}
	pthread_detach(tid);
	started = 1;
}

void perf_add(const struct perf_pic *p)
{
	if (!started)
		return;
	pthread_mutex_lock(&lock);
	if (count == RING) {
		lost++;
	} else {
		ring[(head + count) % RING] = *p;
		count++;
	}
	pthread_mutex_unlock(&lock);
	pthread_cond_signal(&cond);
}

void perf_idle(unsigned us)
{
	pthread_mutex_lock(&lock);
	idle_us += us;
	pthread_mutex_unlock(&lock);
}

void perf_flush(void)
{
	if (!started)
		return;
	pthread_mutex_lock(&lock);
	flush_req = 1;
	pthread_mutex_unlock(&lock);
	pthread_cond_signal(&cond);
}

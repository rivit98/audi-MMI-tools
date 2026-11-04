/*
 * cpumon.c - CPU load of this app and of the unit (see cpumon.h).
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/syspage.h>
#include <time.h>
#include <unistd.h>

#include "cpumon.h"
#include "cpustat.h"
#include "log.h"

#define SAMPLE_MS 1000
#define NAME_LEN 12

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond;
static int             stop_req, started;
static pthread_t       tid;
/* lock: */
static struct { int tid; char name[NAME_LEN]; } names[CPUSTAT_THREADS];
static unsigned        n_names;
static int             now_self = -1, now_unit = -1, now_high;

/* --- sampler thread only ------------------------------------------------- */
static int             self_fd = -1, kern_fd = -1;
static unsigned        ncpu = 1;
static unsigned        window_ms, warn_pct;

struct snap {
	unsigned long long wall, self, idle;      /* us */
	int                idle_ok;
	unsigned           n;
	struct cpustat_thread thr[CPUSTAT_THREADS];   /* run time so far */
};

void cpumon_name_thread(const char *name)
{
	int me = (int)pthread_self();
	unsigned i;

	pthread_mutex_lock(&lock);
	for (i = 0; i < n_names && names[i].tid != me; i++)
		;
	if (i < CPUSTAT_THREADS) {
		names[i].tid = me;
		snprintf(names[i].name, sizeof(names[i].name), "%s", name);
		if (i == n_names)
			n_names++;
	}
	pthread_mutex_unlock(&lock);
}

/* Only from the sampler while it formats (lock held there). */
static const char *name_of(int t)
{
	unsigned i;

	for (i = 0; i < n_names; i++)
		if (names[i].tid == t)
			return names[i].name;
	return NULL;
}

void cpumon_now(int *self_pct, int *unit_pct, int *high)
{
	pthread_mutex_lock(&lock);
	if (self_pct != NULL)
		*self_pct = now_self;
	if (unit_pct != NULL)
		*unit_pct = now_unit;
	if (high != NULL)
		*high = now_high;
	pthread_mutex_unlock(&lock);
}

static unsigned long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}

/* EOK, or devctl's error for our own process */
static int take(struct snap *s)
{
	procfs_info info;
	procfs_status st;
	unsigned cpu;
	int t = 1, rc;

	s->wall = now_us();
	memset(&info, 0, sizeof(info));
	rc = devctl(self_fd, DCMD_PROC_INFO, &info, sizeof(info), NULL);
	if (rc != EOK)
		return rc;
	s->self = (info.utime + info.stime) / 1000u;
	/* The threads, in tid order: TIDSTATUS answers for the given tid or
	 * the next one up, and fails past the last. */
	for (s->n = 0; s->n < CPUSTAT_THREADS; s->n++) {
		memset(&st, 0, sizeof(st));
		st.tid = t;
		if (devctl(self_fd, DCMD_PROC_TIDSTATUS, &st, sizeof(st), NULL) != EOK)
			break;
		s->thr[s->n].tid = (int)st.tid;
		s->thr[s->n].us = st.sutime / 1000u;
		t = (int)st.tid + 1;
	}
	s->idle = 0;
	s->idle_ok = kern_fd >= 0;
	for (cpu = 1; s->idle_ok && cpu <= ncpu; cpu++) {
		memset(&st, 0, sizeof(st));
		st.tid = (pthread_t)cpu;              /* procnto's idle thread(s) */
		if (devctl(kern_fd, DCMD_PROC_TIDSTATUS, &st, sizeof(st), NULL) != EOK ||
		    st.tid != (pthread_t)cpu)
			s->idle_ok = 0;
		else
			s->idle += st.sutime / 1000u;
	}
	return EOK;
}

/* b - a as a sample. A tid not in a (new thread) counts from 0; one whose
 * time went down was reused by a new thread, also from 0. */
static void diff(const struct snap *a, const struct snap *b, struct cpustat_sample *out)
{
	unsigned i, j;

	memset(out, 0, sizeof(*out));
	out->wall_us = b->wall - a->wall;
	out->self_us = b->self >= a->self ? b->self - a->self : 0;
	if (a->idle_ok && b->idle_ok && b->idle >= a->idle) {
		unsigned long long idle = b->idle - a->idle, cap = out->wall_us * ncpu;

		out->unit_us = (long long)((idle < cap ? cap - idle : 0) / ncpu);
	} else {
		out->unit_us = -1;
	}
	for (i = 0; i < b->n; i++) {
		unsigned long long was = 0;

		for (j = 0; j < a->n; j++)
			if (a->thr[j].tid == b->thr[i].tid) {
				was = a->thr[j].us <= b->thr[i].us ? a->thr[j].us : 0;
				break;
			}
		out->thr[out->n].tid = b->thr[i].tid;
		out->thr[out->n].us = b->thr[i].us - was;
		out->n++;
	}
}

static void lower_priority(void)
{
	struct sched_param sp;
	int policy;

	if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && sp.sched_priority > 1) {
		sp.sched_priority--;
		pthread_setschedparam(pthread_self(), policy, &sp);
	}
}

static void *sampler(void *arg)
{
	static struct snap a, b;          /* not on the thread's stack */
	struct cpustat_sample s;
	struct cpustat_win w;
	struct timespec ts;
	char line[384];
	int stop = 0, rc;

	(void)arg;
	lower_priority();
	cpumon_name_thread("cpumon");
	cpustat_reset(&w);
	if ((rc = take(&a)) != EOK) {
		log_msg("cpu: procfs read failed (%s), no CPU monitor", strerror(rc));
		return NULL;
	}
	while (!stop) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_sec += SAMPLE_MS / 1000;
		pthread_mutex_lock(&lock);
		while (!stop_req)
			if (pthread_cond_timedwait(&cond, &lock, &ts) == ETIMEDOUT)
				break;
		stop = stop_req;
		pthread_mutex_unlock(&lock);
		if ((rc = take(&b)) != EOK) {
			log_msg("cpu: procfs read failed (%s), monitor stops", strerror(rc));
			pthread_mutex_lock(&lock);
			now_self = now_unit = -1;
			pthread_mutex_unlock(&lock);
			break;
		}
		diff(&a, &b, &s);
		a = b;
		cpustat_add(&w, &s);
		pthread_mutex_lock(&lock);
		now_self = (int)cpustat_pct(s.self_us, s.wall_us);
		now_unit = s.unit_us < 0 ? -1 : (int)cpustat_pct((unsigned long long)s.unit_us, s.wall_us);
		if (w.wall_us >= window_ms * 1000ull || (stop && w.samples > 0)) {
			unsigned self = cpustat_self_pct(&w);

			cpustat_format(&w, line, sizeof(line), name_of);
			now_high = self >= warn_pct;
			pthread_mutex_unlock(&lock);
			log_msg("cpu: %u.%u s: %s%s", (unsigned)(w.wall_us / 1000000u),
			        (unsigned)(w.wall_us / 100000u % 10), line,
			        self >= warn_pct ? " - HIGH" : "");
			cpustat_reset(&w);
		} else {
			pthread_mutex_unlock(&lock);
		}
	}
	return NULL;
}

int cpumon_start(unsigned window, unsigned warn)
{
	pthread_condattr_t ca;
	char path[32];
	int rc;

	window_ms = window;
	warn_pct = warn;
	snprintf(path, sizeof(path), "/proc/%d/as", (int)getpid());
	self_fd = open(path, O_RDONLY);
	if (self_fd < 0) {
		log_msg("cpu: cannot open %s: %s (no CPU monitor)", path, strerror(errno));
		return -1;
	}
	kern_fd = open("/proc/1/as", O_RDONLY);
	if (kern_fd < 0)
		log_msg("cpu: cannot open /proc/1/as: %s (no unit load)", strerror(errno));
	if (_syspage_ptr != NULL && _syspage_ptr->num_cpu > 0)
		ncpu = _syspage_ptr->num_cpu;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&cond, &ca);
	pthread_condattr_destroy(&ca);
	rc = pthread_create(&tid, NULL, sampler, NULL);
	if (rc != 0) {
		log_msg("cpu: monitor thread: %s", strerror(rc));
		return -1;
	}
	started = 1;
	log_msg("cpu: monitor on, %u CPU(s), a line every %u s, HIGH at %u%% own load",
	        ncpu, window / 1000, warn);
	return 0;
}

void cpumon_stop(void)
{
	if (!started)
		return;
	pthread_mutex_lock(&lock);
	stop_req = 1;
	pthread_mutex_unlock(&lock);
	pthread_cond_signal(&cond);
	pthread_join(tid, NULL);              /* logs the last window */
	started = 0;
	close(self_fd);
	if (kern_fd >= 0)
		close(kern_fd);
}

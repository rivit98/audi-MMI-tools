/*
 * cpustat.h - CPU-load bookkeeping for cpumon.c, kept free of QNX calls so
 * the host tests can run it (tests/test_cpustat.c).
 *
 * cpumon takes one sample a second: what the wall clock, this process
 * and each of its threads ran, and what the whole unit ran (everything but
 * the kernel's idle thread). A window adds samples up and keeps the
 * busiest one; cpustat_format() turns it into the "cpu:" log line.
 */
#ifndef COMMON_CPUSTAT_H
#define COMMON_CPUSTAT_H

#include "types.h"
#include <stddef.h>

#define CPUSTAT_THREADS 32

struct cpustat_thread {
	int                tid;
	unsigned long long us;
};

/* One sample period. */
struct cpustat_sample {
	unsigned long long wall_us;
	unsigned long long self_us;        /* this process, all its threads */
	long long          unit_us;        /* the whole unit busy; < 0 = unknown */
	unsigned           n;              /* threads that ran (or exist) */
	struct cpustat_thread thr[CPUSTAT_THREADS];
};

struct cpustat_win {
	unsigned long long wall_us, self_us, unit_us;
	unsigned           samples;
	unsigned           max_self, max_unit;   /* % of the busiest sample */
	int                unit_known;           /* every sample had unit_us */
	unsigned           n;
	struct cpustat_thread thr[CPUSTAT_THREADS];   /* totals per tid */
};

/* part of whole in %, rounded; 0 if whole is 0, at most 100. */
unsigned cpustat_pct(unsigned long long part, unsigned long long whole);

void cpustat_reset(struct cpustat_win *w);
void cpustat_add(struct cpustat_win *w, const struct cpustat_sample *s);

/* Window's own load in % (what the warning compares). */
unsigned cpustat_self_pct(const struct cpustat_win *w);

/* "self 27% (max 41%), unit 55% (max 90%), others 28% | main 18%, usb 3%"
 * - max = the busiest 1 s sample; others = unit - self; threads busiest
 * first, those that rounded to 0 % left out. name() gives a thread's name
 * (NULL: shown as "tid N"). Returns the length (snprintf-style, cut to
 * cap). */
int cpustat_format(const struct cpustat_win *w, char *out, usize cap,
                   const char *(*name)(int tid));

#endif

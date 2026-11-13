/*
 * cpustat.c - CPU-load windows and the "cpu:" line (see cpustat.h).
 */
#include "types.h"
#include <stdio.h>
#include <string.h>

#include "cpustat.h"

unsigned cpustat_pct(unsigned long long part, unsigned long long whole)
{
	unsigned long long p;

	if (whole == 0)
		return 0;
	p = (part * 100 + whole / 2) / whole;
	return p > 100 ? 100 : (unsigned)p;
}

void cpustat_reset(struct cpustat_win *w)
{
	memset(w, 0, sizeof(*w));
	w->unit_known = 1;
}

void cpustat_add(struct cpustat_win *w, const struct cpustat_sample *s)
{
	unsigned i, j, p;

	w->samples++;
	w->wall_us += s->wall_us;
	w->self_us += s->self_us;
	p = cpustat_pct(s->self_us, s->wall_us);
	if (p > w->max_self)
		w->max_self = p;
	if (s->unit_us < 0) {
		w->unit_known = 0;
	} else {
		w->unit_us += (unsigned long long)s->unit_us;
		p = cpustat_pct((unsigned long long)s->unit_us, s->wall_us);
		if (p > w->max_unit)
			w->max_unit = p;
	}
	for (i = 0; i < s->n && i < CPUSTAT_THREADS; i++) {
		for (j = 0; j < w->n && w->thr[j].tid != s->thr[i].tid; j++)
			;
		if (j == w->n) {
			if (w->n == CPUSTAT_THREADS)
				continue;          /* full: a newcomer goes uncounted */
			w->thr[j].tid = s->thr[i].tid;
			w->thr[j].us = 0;
			w->n++;
		}
		w->thr[j].us += s->thr[i].us;
	}
}

unsigned cpustat_self_pct(const struct cpustat_win *w)
{
	return cpustat_pct(w->self_us, w->wall_us);
}

int cpustat_format(const struct cpustat_win *w, char *out, usize cap,
                   const char *(*name)(int tid))
{
	struct cpustat_thread t[CPUSTAT_THREADS], tmp;
	unsigned i, j, n = w->n, self = cpustat_self_pct(w);
	int len;

	if (w->unit_known && w->samples > 0) {
		unsigned unit = cpustat_pct(w->unit_us, w->wall_us);

		len = snprintf(out, cap, "self %u%% (max %u%%), unit %u%% (max %u%%), others %u%%",
		               self, w->max_self, unit, w->max_unit, unit > self ? unit - self : 0);
	} else {
		len = snprintf(out, cap, "self %u%% (max %u%%), unit ?", self, w->max_self);
	}
	memcpy(t, w->thr, n * sizeof(t[0]));
	for (i = 1; i < n; i++)                    /* busiest first */
		for (j = i; j > 0 && t[j].us > t[j - 1].us; j--) {
			tmp = t[j];
			t[j] = t[j - 1];
			t[j - 1] = tmp;
		}
	for (i = 0; i < n; i++) {
		unsigned p = cpustat_pct(t[i].us, w->wall_us);
		const char *nm = name != NULL ? name(t[i].tid) : NULL;

		if (p == 0 || len < 0 || (usize)len >= cap)
			break;
		if (nm != NULL)
			len += snprintf(out + len, cap - len, "%s%s %u%%", i ? ", " : " | ", nm, p);
		else
			len += snprintf(out + len, cap - len, "%stid %d %u%%", i ? ", " : " | ",
			                t[i].tid, p);
	}
	return len;
}

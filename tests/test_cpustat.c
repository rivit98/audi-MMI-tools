/* test_cpustat.c - CPU-load windows and the "cpu:" line (src/common/cpustat.c). */
#include <string.h>

#include "check.h"
#include "cpustat.h"

static const char *names(int tid)
{
	return tid == 1 ? "main" : tid == 3 ? "usb" : NULL;
}

static void sample(struct cpustat_sample *s, unsigned self_ms, long long unit_ms)
{
	memset(s, 0, sizeof(*s));
	s->wall_us = 1000000;
	s->self_us = self_ms * 1000ull;
	s->unit_us = unit_ms < 0 ? -1 : unit_ms * 1000;
}

static void test_pct(void)
{
	CHECK_EQ(cpustat_pct(0, 0), 0);
	CHECK_EQ(cpustat_pct(5, 1000), 1);           /* 0.5 % rounds up */
	CHECK_EQ(cpustat_pct(4, 1000), 0);
	CHECK_EQ(cpustat_pct(270, 1000), 27);
	CHECK_EQ(cpustat_pct(2000, 1000), 100);      /* clamped */
}

static void test_window(void)
{
	struct cpustat_win w;
	struct cpustat_sample s;
	char line[256];

	cpustat_reset(&w);
	sample(&s, 200, 500);
	s.n = 3;
	s.thr[0].tid = 1; s.thr[0].us = 150000;
	s.thr[1].tid = 3; s.thr[1].us = 40000;
	s.thr[2].tid = 7; s.thr[2].us = 10000;
	cpustat_add(&w, &s);
	sample(&s, 400, 900);                        /* the busiest second */
	s.n = 2;
	s.thr[0].tid = 3; s.thr[0].us = 380000;
	s.thr[1].tid = 9; s.thr[1].us = 2000;       /* new thread, 0.1 % in 2 s */
	cpustat_add(&w, &s);
	CHECK_EQ(w.samples, 2);
	CHECK_EQ(cpustat_self_pct(&w), 30);
	CHECK_EQ(w.max_self, 40);
	CHECK_EQ(w.max_unit, 90);
	CHECK_EQ(w.n, 4);
	cpustat_format(&w, line, sizeof(line), names);
	/* usb 420 ms / 2 s = 21 %, main 150 = 8 %, tid 7 10 = 1 % (0.5 up), tid 9 0 % left out */
	CHECK_STR(line, "self 30% (max 40%), unit 70% (max 90%), others 40% | usb 21%, main 8%, tid 7 1%");
}

static void test_unit_unknown(void)
{
	struct cpustat_win w;
	struct cpustat_sample s;
	char line[128];

	cpustat_reset(&w);
	sample(&s, 100, 300);
	cpustat_add(&w, &s);
	sample(&s, 100, -1);                         /* one sample without it */
	cpustat_add(&w, &s);
	cpustat_format(&w, line, sizeof(line), NULL);
	CHECK_STR(line, "self 10% (max 10%), unit ?");
}

static void test_cut_short(void)
{
	struct cpustat_win w;
	struct cpustat_sample s;
	char line[40];
	unsigned i;

	cpustat_reset(&w);
	sample(&s, 900, 950);
	s.n = CPUSTAT_THREADS;
	for (i = 0; i < s.n; i++) {
		s.thr[i].tid = (int)i + 1;
		s.thr[i].us = 30000;
	}
	cpustat_add(&w, &s);
	cpustat_format(&w, line, sizeof(line), NULL);
	CHECK(strlen(line) < sizeof(line));          /* no overrun, still a C string */
	CHECK(strncmp(line, "self 90% (max 90%)", 18) == 0);
}

static void test_table_full(void)
{
	struct cpustat_win w;
	struct cpustat_sample s;
	unsigned i;

	cpustat_reset(&w);
	sample(&s, 10, 10);
	s.n = CPUSTAT_THREADS;
	for (i = 0; i < s.n; i++)
		s.thr[i].tid = (int)i + 1;
	cpustat_add(&w, &s);
	s.thr[0].tid = 999;                          /* a newcomer: no room, not counted */
	cpustat_add(&w, &s);
	CHECK_EQ(w.n, CPUSTAT_THREADS);
}

int main(void)
{
	test_pct();
	test_window();
	test_unit_unknown();
	test_cut_short();
	test_table_full();
	CHECK_DONE();
}

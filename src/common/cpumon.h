/*
 * cpumon.h - how much CPU this app takes, and the whole unit, watched by
 * a low-priority thread (QNX procfs; arithmetic in cpustat.c).
 *
 * Once a second it reads this process's CPU time (DCMD_PROC_INFO), each of
 * its threads' (DCMD_PROC_TIDSTATUS on /proc/<pid>/as) and the kernel's
 * idle thread (procnto, pid 1, tids 1..num_cpu): unit load = 1 - idle.
 * Every window_ms it logs
 *
 *   cpu: 10.0 s: self 27% (max 41%), unit 55% (max 90%), others 28% | main 18%, usb 3%
 *
 * (max = the busiest second, others = the rest of the unit, threads named
 * by cpumon_name_thread() or "tid N"), with " - HIGH" at the end when
 * self reached warn_pct. Kernel time is charged a clock tick at a time
 * (1 ms), so single seconds are approximate and windows good.
 */
#ifndef COMMON_CPUMON_H
#define COMMON_CPUMON_H

/* Starts the thread; 0, or -1 (logged) if procfs can't be read. */
int cpumon_start(unsigned window_ms, unsigned warn_pct);
void cpumon_stop(void);

/* Name the calling thread in the "cpu:" lines (max 11 chars kept). */
void cpumon_name_thread(const char *name);

/* The last second: own and unit load in %, -1 = not known (yet). *high:
 * the last window reached warn_pct. Any thread; NULL for what isn't
 * wanted. */
void cpumon_now(int *self_pct, int *unit_pct, int *high);

#endif

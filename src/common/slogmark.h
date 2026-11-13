/*
 * slogmark.h - our own lines in the system log, so a sloginfo snapshot lines
 * up with the app's log. slog stamps its lines with the wall clock (1970
 * after a cold boot) and our log with ms since the app started; a marker
 *
 *   <app>: t=123456 ms <text>
 *
 * carries both. Severity 6 (_SLOG_DEBUG1, the level of most of the unit's own
 * lines, so slogger keeps it), major code SLOGMARK_MAJOR. The unit's libc
 * has no slogf(), so this writes the record to /dev/slog itself (the layout
 * openqnx's slogf() sends: severity | text bit, code, a spare int, the text).
 *
 * One write() to slogger's RAM buffer per marker, no card I/O: cheap, but
 * meant for every few seconds and for events, not per frame.
 */
#ifndef COMMON_SLOGMARK_H
#define COMMON_SLOGMARK_H

#define SLOGMARK_MAJOR 30000

/* Opens /dev/slog for writing; app: the prefix. 0, or -1 (logged): markers
 * are then dropped. Logs where they go. */
int slogmark_init(const char *app);
/* A marker (any thread; at most ~200 characters kept). Nothing before
 * slogmark_init() or after a failed write. */
void slogmark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif

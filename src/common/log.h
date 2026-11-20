/*
 * log.h - line log on the SD card, flushed + fsync'ed after every line so a
 * crash or a pulled card still leaves everything up to the last step.
 */
#ifndef COMMON_LOG_H
#define COMMON_LOG_H

int  log_open(const char *path);

/* "<sd_path>/var", created if missing: where every app writes its logs,
 * screenshots, recordings and other output, so one folder holds a run.
 * The card's root keeps only inputs (bin/, clips/, run.sh ...). A static
 * buffer, the same for the whole program. */
const char *var_dir(const char *sd_path);
void log_close(void);

/* "[   123 ms] message" to the log file and stdout. */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Called with every line ("[   123 ms] message", no newline, at most 255
 * bytes) under the log's lock, in the logging thread: copy it and return;
 * it must not log. NULL: none. */
void log_set_tap(void (*tap)(const char *line));

/* Milliseconds since log_open(), CLOCK_MONOTONIC. */
unsigned log_ms(void);
/* The same in microseconds. Wraps after ~71 min: use differences only. As
 * fine as the system clock (clock_getres; the QNX tick, often 1 ms). */
unsigned log_us(void);

/* "YYYYMMDD-HHMMSS-mmm" (UTC, CLOCK_REALTIME) for output file names. The
 * MMI's clock starts at 1970-01-01 00:00 on every cold boot, so this is the
 * time since boot, not the date: unique and ordered within one boot only.
 * buf: at least 20 bytes. */
void log_stamp(char *buf, unsigned size);

#endif

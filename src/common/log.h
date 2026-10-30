/*
 * log.h - line log on the SD card, flushed + fsync'ed after every line so a
 * crash or a pulled card still leaves everything up to the last step.
 */
#ifndef COMMON_LOG_H
#define COMMON_LOG_H

int  log_open(const char *path);
void log_close(void);

/* "[   123 ms] message" to the log file and stdout. */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Milliseconds since log_open(), CLOCK_MONOTONIC. */
unsigned log_ms(void);
/* The same in microseconds. Wraps after ~71 min: use differences only. As
 * fine as the system clock (clock_getres; the QNX tick, often 1 ms). */
unsigned log_us(void);

#endif

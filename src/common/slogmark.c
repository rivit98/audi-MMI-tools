/*
 * slogmark.c - markers in the system log (see slogmark.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "slogmark.h"

#define SLOG_DEBUG1   6           /* the severity of most lines in the unit's slog */
#define SLOG_TEXTBIT  0x8         /* the data is text (sys/slog.h _SLOG_TEXTBIT) */
#define HDR_INTS      3           /* sys/slog.h _SLOG_HDRINTS */
#define TEXT_MAX      224

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int fd = -1;
static char prefix[32];

int slogmark_init(const char *app)
{
	int f = open("/dev/slog", O_WRONLY);

	if (f < 0) {
		log_msg("slog: cannot open /dev/slog for writing: %s (no markers)", strerror(errno));
		return -1;
	}
	pthread_mutex_lock(&lock);
	fd = f;
	snprintf(prefix, sizeof(prefix), "%s", app);
	pthread_mutex_unlock(&lock);
	log_msg("slog: markers on (\"%s: t=<ms> ms ...\", major %d), to line slog snapshots up "
	        "with this log", app, SLOGMARK_MAJOR);
	return 0;
}

void slogmark(const char *fmt, ...)
{
	int rec[HDR_INTS + TEXT_MAX / sizeof(int)];
	char *text = (char *)&rec[HDR_INTS];
	va_list ap;
	int n, m, failed = 0, err = 0;

	pthread_mutex_lock(&lock);
	if (fd < 0) {
		pthread_mutex_unlock(&lock);
		return;
	}
	rec[0] = SLOG_DEBUG1 | SLOG_TEXTBIT;
	rec[1] = SLOGMARK_MAJOR;          /* _SLOG_SETCODE(major, 0) */
	rec[2] = 0;
	n = snprintf(text, TEXT_MAX, "%s: t=%u ms ", prefix, log_ms());
	if (n < 0 || n >= TEXT_MAX)
		n = 0;
	va_start(ap, fmt);
	m = vsnprintf(text + n, TEXT_MAX - n, fmt, ap);
	va_end(ap);
	if (m < 0)
		m = 0;
	n += m;
	if (n > TEXT_MAX - 1)
		n = TEXT_MAX - 1;
	text[n] = 0;
	if (write(fd, rec, HDR_INTS * sizeof(int) + n + 1) < 0) {
		err = errno;
		close(fd);
		fd = -1;
		failed = 1;
	}
	pthread_mutex_unlock(&lock);
	if (failed)                       /* logged after the lock: log.c fsyncs */
		log_msg("slog: marker write failed: %s (no more markers)", strerror(err));
}

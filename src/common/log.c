#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static FILE *log_file;
static struct timespec log_t0;

static unsigned ms_since(const struct timespec *t0)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)((now.tv_sec - t0->tv_sec) * 1000 +
	                  (now.tv_nsec - t0->tv_nsec) / 1000000);
}

unsigned log_ms(void)
{
	return ms_since(&log_t0);
}

int log_open(const char *path)
{
	clock_gettime(CLOCK_MONOTONIC, &log_t0);
	log_file = fopen(path, "w");
	return log_file != NULL ? 0 : -1;
}

void log_close(void)
{
	if (log_file != NULL) {
		fclose(log_file);
		log_file = NULL;
	}
}

void log_msg(const char *fmt, ...)
{
	unsigned ms = log_ms();
	va_list ap;

	printf("[%6u ms] ", ms);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);

	if (log_file == NULL)
		return;
	fprintf(log_file, "[%6u ms] ", ms);
	va_start(ap, fmt);
	vfprintf(log_file, fmt, ap);
	va_end(ap);
	fputc('\n', log_file);
	fflush(log_file);
	fsync(fileno(log_file));
}

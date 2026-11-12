#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;  /* one line at a time */
static struct timespec log_t0;
static void (*log_tap)(const char *line);

void log_set_tap(void (*tap)(const char *line))
{
	pthread_mutex_lock(&log_lock);
	log_tap = tap;
	pthread_mutex_unlock(&log_lock);
}

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

unsigned log_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)((now.tv_sec - log_t0.tv_sec) * 1000000 +
	                  (now.tv_nsec - log_t0.tv_nsec) / 1000);
}

const char *var_dir(const char *sd_path)
{
	static char dir[256];
	static const char *made_for;   /* mkdir once per sd_path */

	if (sd_path != made_for) {
		snprintf(dir, sizeof(dir), "%s/var", sd_path);
		mkdir(dir, 0777);          /* EEXIST is fine; a real failure shows at fopen */
		made_for = sd_path;
	}
	return dir;
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

	pthread_mutex_lock(&log_lock);
	printf("[%6u ms] ", ms);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);

	if (log_tap != NULL) {
		char line[256];
		int n = snprintf(line, sizeof(line), "[%6u ms] ", ms);
		va_start(ap, fmt);
		vsnprintf(line + n, sizeof(line) - n, fmt, ap);
		va_end(ap);
		log_tap(line);
	}

	if (log_file == NULL) {
		pthread_mutex_unlock(&log_lock);
		return;
	}
	fprintf(log_file, "[%6u ms] ", ms);
	va_start(ap, fmt);
	vfprintf(log_file, fmt, ap);
	va_end(ap);
	fputc('\n', log_file);
	fflush(log_file);
	fsync(fileno(log_file));
	pthread_mutex_unlock(&log_lock);
}

/*
 * pidscan.c - see pidscan.h.
 */
#include "pidscan.h"

#include <stdio.h>
#include <string.h>

/* A decimal number at *p (no sign); advances p. -1 if none or too big. */
static i32 number(const char **p)
{
	const char *s = *p;
	i32 v = 0;

	if (*s < '0' || *s > '9')
		return -1;
	while (*s >= '0' && *s <= '9') {
		if (v > 99999999)
			return -1;
		v = v * 10 + (*s - '0');
		s++;
	}
	*p = s;
	return v;
}

/* Does the word starting at s equal name, ending in a space or the end? */
static int word_is(const char *s, const char *name)
{
	usize n = strlen(name);

	return strncmp(s, name, n) == 0 &&
	       (s[n] == '\0' || s[n] == ' ' || s[n] == '\t' || s[n] == '\n' ||
	        s[n] == '\r');
}

/* "pid N: application NAME started" / "pid N: process NAME started" */
static i32 slog_line(const char *line, const char *name)
{
	const char *p = line;
	static const char *const kinds[] = { "application ", "process " };
	unsigned k;
	i32 pid;

	while ((p = strstr(p, "pid ")) != NULL) {
		const char *q = p + 4;

		p += 4;
		pid = number(&q);
		if (pid <= 1 || strncmp(q, ": ", 2) != 0)
			continue;
		q += 2;
		for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
			usize n = strlen(kinds[k]);

			if (strncmp(q, kinds[k], n) == 0 && word_is(q + n, name) &&
			    strncmp(q + n + strlen(name), " started", 8) == 0)
				return pid;
		}
	}
	return 0;
}

/* "  N /path/NAME args" */
static i32 pidin_line(const char *line, const char *name)
{
	const char *p = line, *w, *base;
	i32 pid;

	while (*p == ' ' || *p == '\t')
		p++;
	pid = number(&p);
	if (pid <= 1 || (*p != ' ' && *p != '\t'))
		return 0;
	while (*p == ' ' || *p == '\t')
		p++;
	/* argv[0]: up to the first blank; its basename */
	base = p;
	for (w = p; *w != '\0' && *w != ' ' && *w != '\t' && *w != '\n' &&
	     *w != '\r'; w++)
		if (*w == '/')
			base = w + 1;
	return word_is(base, name) ? pid : 0;
}

i32 pidscan_line(const char *line, const char *name)
{
	i32 pid;

	if (line == NULL || name == NULL || name[0] == '\0')
		return 0;
	pid = slog_line(line, name);
	return pid != 0 ? pid : pidin_line(line, name);
}

i32 pidscan_file(const char *path, const char *name)
{
	char line[512];
	FILE *f = fopen(path, "r");
	i32 pid, last = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		pid = pidscan_line(line, name);
		if (pid != 0)
			last = pid;
	}
	fclose(f);
	return last;
}

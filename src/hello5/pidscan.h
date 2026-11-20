/*
 * pidscan.h - a process's pid from the text of the unit's own tools, for
 * when /proc does not give it. Pure: no QNX calls, host-tested
 * (tests/test_pidscan.c).
 *
 * Two sources, one line at a time:
 *  - sloginfo: srv-starter and MMI3GApplication log every start,
 *      "pid 172057: application MMI3GApplication started (watchdog ...)"
 *      "pid 172074: process MMI3GNavigation started (watchdog ...)"
 *  - `pidin arg`: "  172057 /usr/apps/MMI3GApplication -x ..."
 *    (pid, then argv[0], whose basename is the name).
 * The last matching line wins: a restarted process logs its new pid later.
 */
#ifndef HELLO5_PIDSCAN_H
#define HELLO5_PIDSCAN_H

#include "types.h"

/* The pid line names for `name`, or 0 if it names none. */
i32 pidscan_line(const char *line, const char *name);

/* The last pid any line of the file at path names for `name`, or 0 (also
 * when the file cannot be read). */
i32 pidscan_file(const char *path, const char *name);

#endif

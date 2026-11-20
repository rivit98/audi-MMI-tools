/*
 * test_pidscan.c - hello5's pid fallback (src/hello5/pidscan.c): pids out
 * of sloginfo and `pidin arg` lines, as the unit prints them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "check.h"
#include "pidscan.h"

static void test_slog(void)
{
	CHECK_EQ(pidscan_line("Jan 01 00:00:05    5 10000     0 pid 172057: application "
	                      "MMI3GApplication started (watchdog is enabled)",
	                      "MMI3GApplication"), 172057);
	CHECK_EQ(pidscan_line("Jan 01 00:00:07    5 10000     0 pid 172074: process "
	                      "MMI3GNavigation started (watchdog is enabled)",
	                      "MMI3GNavigation"), 172074);
	/* the spawning line names the parent, not the child */
	CHECK_EQ(pidscan_line("Jan 01 00:00:06    5 10000     0 pid 172057: spawning "
	                      "child process MMI3GNavigation", "MMI3GNavigation"), 0);
	/* other process, prefix of a name */
	CHECK_EQ(pidscan_line("pid 5: application MMI3GApplication started",
	                      "MMI3GNavigation"), 0);
	CHECK_EQ(pidscan_line("pid 5: application MMI3GApplicationX started",
	                      "MMI3GApplication"), 0);
	/* srv-starter's own lines carry no pid */
	CHECK_EQ(pidscan_line("Jan 01 00:00:00    6 20000     0 Process[95]: 'RUN' "
	                      "'/usr/bin/servicebroker'", "servicebroker"), 0);
}

static void test_pidin(void)
{
	CHECK_EQ(pidscan_line("  172057 /usr/apps/MMI3GApplication -c x\n",
	                      "MMI3GApplication"), 172057);
	CHECK_EQ(pidscan_line("     45 usr/bin/servicebroker\n", "servicebroker"), 45);
	CHECK_EQ(pidscan_line("4100 servicebroker", "servicebroker"), 4100);
	CHECK_EQ(pidscan_line("     pid Arguments\n", "servicebroker"), 0);
	CHECK_EQ(pidscan_line("  12 /usr/bin/servicebroker2\n", "servicebroker"), 0);
	CHECK_EQ(pidscan_line("  12 /usr/bin/x servicebroker\n", "servicebroker"), 0);
	CHECK_EQ(pidscan_line("  1 procnto\n", "procnto"), 0);
	CHECK_EQ(pidscan_line("", "x"), 0);
	CHECK_EQ(pidscan_line(NULL, "x"), 0);
	CHECK_EQ(pidscan_line("  12 x", ""), 0);
}

static void test_file(void)
{
	char path[] = "/tmp/test_pidscan_XXXXXX";
	int fd = mkstemp(path);
	FILE *f;

	CHECK(fd >= 0);
	if (fd < 0)
		return;
	f = fdopen(fd, "w");
	fputs("### $ sloginfo\n"
	      "pid 100: application MMI3GApplication started (watchdog)\n"
	      "pid 101: process MMI3GNavigation started (watchdog)\n"
	      "pid 200: application MMI3GApplication started (watchdog)\n"
	      "### exit 0\n", f);
	fclose(f);
	CHECK_EQ(pidscan_file(path, "MMI3GApplication"), 200);   /* the last one */
	CHECK_EQ(pidscan_file(path, "MMI3GNavigation"), 101);
	CHECK_EQ(pidscan_file(path, "servicebroker"), 0);
	unlink(path);
	CHECK_EQ(pidscan_file(path, "MMI3GApplication"), 0);
}

int main(void)
{
	test_slog();
	test_pidin();
	test_file();
	CHECK_DONE();
}

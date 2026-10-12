/*
 * sysinfo.c - what the unit says about itself, into hello6/<file>: process
 * list and CPU times, memory, the io-media and audio device nodes, where
 * the efs tools live, and the GPU state from the unit's own diagnostic
 * tools (ShowVideoMemory, memusage, hbhogs).
 *
 * Not ShowDisplayables: it is not a query. `ShowDisplayables <display>
 * [cid...]` SETS the shown list, so `ShowDisplayables 0` blanked the whole
 * screen for the rest of the phase (12.10 v2 run, phone video).
 *
 * Everything runs through `ksh -c` with a timeout, so a tool that hangs or
 * wants a terminal costs at most that timeout.
 */
#include <stdio.h>

#include "hello6.h"
#include "log.h"
#include "proc.h"
#include "quit.h"

static void tool(const char *file, unsigned timeout_s, const char *name,
                 const char *args)
{
	const char *p = tool_path(name);
	char cmd[512];

	if (p == NULL) {
		FILE *f = fopen(file, "a");

		if (f != NULL) {
			fprintf(f, "\n### %s: not found on the unit or the card\n", name);
			fclose(f);
		}
		return;
	}
	snprintf(cmd, sizeof(cmd), "%s %s", p, args);
	proc_shell(file, timeout_s, cmd);
}

void sysinfo_run(const char *file_name, int full)
{
	static const char *const cmds[] = {
		"uname -a",
		"pidin info",
		"pidin -p io-media-nvidia",
		"pidin -p io-media-nvidia mem",
		"pidin times",
		"pidin mem",
		"mount",
		"ls -l /dev /dev/io-media /dev/io-media/graphs /dev/io-media/controls /dev/snd",
		"ls -lR /dev/io-media",
		"ls -l /mnt/efs-system /mnt/efs-system/bin /mnt/efs-system/usr/bin /mnt/efs-extended",
		"ls -l /dev/shmem",
		"showmem",
	};
	char file[256];
	unsigned i, t0 = log_ms();

	out_path(file, sizeof(file), file_name);
	log_msg("== system info (%s) -> %s", full ? "full" : "GPU only", file);
	for (i = 0; full && i < sizeof(cmds) / sizeof(cmds[0]) && quit_poll() == QUIT_NONE; i++)
		proc_shell(file, 10, cmds[i]);

	tool(file, 10, "ShowVideoMemory", "");
	tool(file, 10, "memusage", "");
	if (!full)
		return;
	/* hbhogs samples forever: 2 s intervals, stopped after the timeout */
	tool(file, 7, "hbhogs", "-t 2000 -n 25");
	/* no URL: creates an io-media graph, attaches the default audio
	 * output, says "No files to play" and destroys it - io-media smoke test */
	tool(file, 5, "testapp-file", "");
	log_msg("system info done in %u ms", log_ms() - t0);
}

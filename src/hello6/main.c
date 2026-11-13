/*
 * hello6 - checks the assumptions behind phone screen mirroring
 * (docs/phone-mirroring.md) on the car, in four phases:
 *
 *   s  system info: processes, memory, device nodes, efs tool locations,
 *      GPU memory and layer manager state (sysinfo.c)
 *   c  CPU: memcpy, RGB565->BGRA, LZ4 on UI/photo/random frames (bench.c)
 *   v  video: clips through the unit's DM642 decoder via io-media and
 *      testapp-file, shown on a layer manager video displayable with the
 *      capture unit enabled through dev-videoctrl; the driver turns the
 *      knob when the picture is visible (video.c). All ten clips. (The
 *      streaming tests that followed from them are hello9's.)
 *   g  GPU: CPU writes to the frame buffer, texture upload (RGBA, BGRA,
 *      RGB565, DXT1), visible upload+draw pipelines in fps (bench.c)
 *
 * The phases always run in this order, g last: the first full-frame
 * texture upload can freeze the whole unit, which costs the video phase.
 *
 * Text screens (cid 40) say what is happening and what to do. A summary
 * screen at the end shows the key numbers for 20 s.
 *
 * Output: <sd>/hello6.log (every step, "RESULT" lines, the summary) and
 * <sd>/hello6/ (sysinfo, per-clip player logs and io-media queries, CPU
 * load, the system log right after the video phase).
 *
 * The unit's tools come from its efs (/mnt/efs-system/bin, served
 * decompressed by inflator) or, failing that, from the copies `make sd`
 * puts next to hello6 on the card.
 *
 * Default: v only, every clip through both capture paths (~6 min). The
 * rest only costs the driver's time (and g can freeze the unit).
 *
 * usage: hello6 <sd_path> [phases]     phases: any of "scgv" (default "v")
 */
#include "types.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "hello6.h"
#include "lmgr.h"
#include "log.h"
#include "proc.h"
#include "quit.h"
#include "texts.h"

#define WATCHDOG_S   900     /* v alone ~6 min (10 clips x 30-38 s) */
#define INTRO_MS     12000   /* 8 s is too short */
#define PAGE_MS      12000   /* per results page */
#define MAX_SUMMARY  48

static const char *sd_path;
static char bin_dir[200];
static volatile unsigned knob_count;
static char *summary[MAX_SUMMARY];
static int nsummary;

const char *sd_card_path(void)
{
	return sd_path;
}

void out_path(char *buf, usize n, const char *name)
{
	snprintf(buf, n, "%s/hello6/%s", var_dir(sd_path), name);
}

void summary_add(const char *fmt, ...)
{
	char line[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (nsummary < MAX_SUMMARY)
		summary[nsummary++] = strdup(line);
}

int sleep_quit(unsigned ms)
{
	unsigned t0 = log_ms();

	while (quit_poll() == QUIT_NONE) {
		unsigned left, now = log_ms();

		if (now - t0 >= ms)
			return 0;
		left = ms - (now - t0);
		usleep((left > 100 ? 100 : left) * 1000);
	}
	return 1;
}

unsigned knob_events(void)
{
	return knob_count;
}

static void on_key(const unsigned char *buf, int n, void *arg)
{
	(void)buf;
	(void)n;
	(void)arg;
	knob_count++;
}

/* ------------------------------------------------------------------ */
/* the unit's tools                                                    */

static const char *const tool_names[] = {
	"testapp-file", "testapp-cmdline", "mmecli", "ShowVideoMemory", "memusage",
	"hbhogs",
};
#define NTOOLS ((int)(sizeof(tool_names) / sizeof(tool_names[0])))
static char tool_found[NTOOLS][300];

static void find_tools(void)
{
	static const char *const dirs[] = {
		"/mnt/efs-system/bin", "/mnt/efs-extended", "/mnt/efs-system/usr/bin",
		"/mnt/efs-system/sbin", "/usr/bin", "/bin",
	};
	int t, d;

	log_msg("tools (unit first, then the card's copy in %s):", bin_dir);
	for (t = 0; t < NTOOLS; t++) {
		char p[300];

		tool_found[t][0] = 0;
		for (d = 0; d < (int)(sizeof(dirs) / sizeof(dirs[0])); d++) {
			snprintf(p, sizeof(p), "%s/%s", dirs[d], tool_names[t]);
			if (access(p, X_OK) == 0) {
				snprintf(tool_found[t], sizeof(tool_found[t]), "%s", p);
				break;
			}
		}
		if (tool_found[t][0] == 0) {
			snprintf(p, sizeof(p), "%s/%s", bin_dir, tool_names[t]);
			if (access(p, X_OK) == 0)
				snprintf(tool_found[t], sizeof(tool_found[t]), "%s", p);
		}
		log_msg("  %-17s %s", tool_names[t], tool_found[t][0] ? tool_found[t] : "NOT FOUND");
	}
}

const char *tool_path(const char *name)
{
	int t;

	for (t = 0; t < NTOOLS; t++)
		if (strcmp(tool_names[t], name) == 0)
			return tool_found[t][0] ? tool_found[t] : NULL;
	return NULL;
}

/* ------------------------------------------------------------------ */

/* Whatever happened, leave no displayable of ours behind and give the
 * screen back to the HMI (showScreen's exit: show {40,16,19}). */
static void restore_screen(void)
{
	int cids[3] = { 40, 16, 19 };

	label_hide();
	if (lm_open() != 0)
		return;
	lm_unregister_displayable(41);
	lm_unregister_displayable(40);
	log_msg("restore: lm_show_displayables(0, {40,16,19}): rc=%d",
	        lm_show_displayables(0, cids, 3));
	lm_close();
}

int main(int argc, char *argv[])
{
	const char *phases = "v";
	char path[300], *slash;
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [phases: any of scgv, default v]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	if (argc > 2 && argv[2][0] != 0)
		phases = argv[2];
	snprintf(bin_dir, sizeof(bin_dir), "%s", argv[0]);
	slash = strrchr(bin_dir, '/');
	if (slash != NULL)
		*slash = 0;
	else
		snprintf(bin_dir, sizeof(bin_dir), ".");

	snprintf(path, sizeof(path), "%s/hello6", var_dir(sd_path));
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/hello6.log", var_dir(sd_path));
	if (log_open(path) != 0)
		fprintf(stderr, "hello6: cannot open %s, logging to stdout only\n", path);
	log_msg("hello6 start, pid=%d, phases=%s", getpid(), phases);
	quit_init_ex(sd_path, WATCHDOG_S, on_key, NULL);
	find_tools();

	label_show(INTRO_TITLE, INTRO_BODY, FOOTER_MAIN);
	if (sleep_quit(INTRO_MS))
		goto out;

	if (strchr(phases, 's') != NULL) {
		label_show(SYSINFO_TITLE, SYSINFO_BODY, NULL);
		sysinfo_run("sysinfo_before.txt", 1);
	}
	if (strchr(phases, 'c') != NULL && quit_poll() == QUIT_NONE) {
		label_show(CPU_TITLE, CPU_BODY, NULL);
		bench_cpu();
	}
	label_hide();
	if (strchr(phases, 'v') != NULL && quit_poll() == QUIT_NONE) {
		video_tests();
		/* io-media's own messages for the clips. run.sh saves the slog
		 * after the app, but when the GPU phase freezes the unit
		 * first that copy is never made. */
		out_path(path, sizeof(path), "slog_video.txt");
		log_msg("== system log after the video tests -> %s: %s", path,
		        proc_status_str(proc_shell(path, 30, "sloginfo")));
	}
	if (strchr(phases, 's') != NULL && quit_poll() == QUIT_NONE)
		sysinfo_run("sysinfo_after.txt", 0);
	if (strchr(phases, 'g') != NULL && quit_poll() == QUIT_NONE)
		bench_gpu();               /* its own displayable: test patterns */

	log_msg("== summary");
	for (i = 0; i < nsummary; i++)
		log_msg("  %s", summary[i]);
	/* as many pages as the lines need, PAGE_MS each */
	for (i = 0; i < nsummary && quit_poll() == QUIT_NONE;) {
		int shown;

		label_show_list(SUMMARY_TITLE, (const char *const *)summary + i, nsummary - i);
		shown = nsummary - i - label_dropped();
		if (shown <= 0)
			break;
		i += shown;
		sleep_quit(PAGE_MS);
	}
out:
	restore_screen();
	log_msg("hello6 exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

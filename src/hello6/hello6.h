/*
 * hello6.h - shared by hello6's parts (see main.c for what hello6 does).
 */
#ifndef HELLO6_H
#define HELLO6_H

#include <stddef.h>

#define SCREEN_W 800
#define SCREEN_H 480

/* The SD card root (argv[1]). */
const char *sd_card_path(void);

/* <sd>/hello6/<name>: every file hello6 writes besides hello6.log. */
void out_path(char *buf, size_t n, const char *name);

/* A line for the end-of-run summary (log + summary screen). */
void summary_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Sleep up to ms, waking early on a stop request. Returns non-zero if
 * hello6 should stop. */
int sleep_quit(unsigned ms);

/* Messages seen on /dev/ipc/ch8 (knob turns, button presses) so far. */
unsigned knob_events(void);

/* Path of one of the unit's tools (testapp-file, hbhogs, ...):
 * the unit's own copy if there is one, else the copy hello6 brought on the
 * card. NULL if neither exists. Logged once at start-up. */
const char *tool_path(const char *name);

/* phases */
/* System + GPU info into hello6/<file>; full = 0: only the GPU memory and
 * layer manager tools (for an "after" snapshot). */
void sysinfo_run(const char *file_name, int full);
void bench_cpu(void);
void bench_gpu(void);
void video_tests(void);

/* Full-screen text screen through the layer manager (cid 40). */
int  label_show(const char *title, const char *body, const char *footer);
/* Title plus n lines in the small font (summary screen). */
int  label_show_list(const char *title, const char *const *lines, int n);
void label_hide(void);
/* Lines the last label_show / label_show_list could not fit on screen. */
int  label_dropped(void);

#endif

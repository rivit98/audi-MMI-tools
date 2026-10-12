/*
 * hello6_preview.c - hello6's instruction screens rendered on the host
 * (make hello6-preview -> build/hello6_*.bmp), with the real label.c,
 * text.c, fonts and texts.h; the layer manager screen (usb_tx_test's
 * screen.c) is replaced by a canvas that screen_present() saves as a BMP.
 * Exits non-zero if any screen has text that does not fit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"
#include "hello6.h"
#include "screen.h"
#include "texts.h"

static struct canvas cv;
static char target[128];
static int failures;

int screen_open(void)
{
	cv.w = SCREEN_W;
	cv.h = SCREEN_H;
	cv.stride = SCREEN_W;
	cv.px = calloc(SCREEN_W * SCREEN_H, 4);
	return cv.px ? 0 : -1;
}

struct canvas *screen_canvas(void)
{
	return &cv;
}

/* BMP rows are bottom-up RGBA; the canvas is top-down 0xAARRGGBB. */
int screen_present(void)
{
	unsigned char *rgba = malloc(SCREEN_W * SCREEN_H * 4);
	int x, y;

	for (y = 0; y < SCREEN_H; y++)
		for (x = 0; x < SCREEN_W; x++) {
			uint32_t p = cv.px[(SCREEN_H - 1 - y) * cv.stride + x];
			unsigned char *o = rgba + (y * SCREEN_W + x) * 4;

			o[0] = (unsigned char)(p >> 16);
			o[1] = (unsigned char)(p >> 8);
			o[2] = (unsigned char)p;
			o[3] = 0xff;
		}
	img_write_bmp_rgba(target, rgba, SCREEN_W, SCREEN_H);
	free(rgba);
	return 0;
}

void screen_close(void)
{
	free(cv.px);
	cv.px = NULL;
}

static void check(const char *name)
{
	if (label_dropped() > 0) {
		fprintf(stderr, "%s: %d line(s) do not fit\n", name, label_dropped());
		failures++;
	}
	printf("  %s%s\n", target, label_dropped() > 0 ? "  (TEXT CUT OFF)" : "");
}

static void show(const char *name, const char *title, const char *body, const char *footer)
{
	snprintf(target, sizeof(target), "build/hello6_%s.bmp", name);
	label_show(title, body, footer);
	check(name);
	label_hide();
}

int main(void)
{
	/* a full-length results page: the longest lines hello6 makes */
	static const char *const results[] = {
		"RAM memcpy         123.4 MB/s",
		"RGB565->BGRA        12.3 ms/frame",
		"lz4 ui     15.1% of size, decode  10.2 ms/frame (72 MB/s)",
		"VFB CPU store32     10.1 MB/s (145 ms/frame)",
		"upload rgb565      123.4 MB/s (5.9 ms/frame)",
		"pipeline rgb565_band120   58.8 fps (worst 34 ms)",
		"video: TIME = io-media progress events, knob = turns seen per VCAP",
		"10 c8_h264_336.mpg       TIME 24 pos 11.5s knob 1/0 reg ok/ok file://",
	};
	char title[96], body[700];
	int k;

	printf("hello6 screens:\n");
	show("intro", INTRO_TITLE, INTRO_BODY, FOOTER_MAIN);
	show("sysinfo", SYSINFO_TITLE, SYSINFO_BODY, NULL);
	show("cpu", CPU_TITLE, CPU_BODY, NULL);
	for (k = 0; k < NCLIPS; k++) {
		char name[32];

		snprintf(name, sizeof(name), "video%d", k + 1);
		snprintf(title, sizeof(title), VIDEO_TITLE_FMT, k + 1, NCLIPS, clips[k].title);
		snprintf(body, sizeof(body), VIDEO_BODY_FMT, clips[k].what);
		show(name, title, body, FOOTER_VIDEO);
	}
	snprintf(target, sizeof(target), "build/hello6_results.bmp");
	label_show_list(SUMMARY_TITLE, results, (int)(sizeof(results) / sizeof(results[0])));
	check("results");
	label_hide();
	return failures ? 1 : 0;
}

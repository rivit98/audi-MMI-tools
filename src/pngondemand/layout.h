/*
 * layout.h - render a text layout onto a canvas (pngondemand's format).
 *
 * The language (commands, fonts, colours, flow text) is described in
 * README.md next to this file; `pngondemand -h` prints a summary.
 */
#ifndef PNGOD_LAYOUT_H
#define PNGOD_LAYOUT_H

#include "text.h"

#define LAYOUT_MAX_W 2048
#define LAYOUT_MAX_H 2048

struct layout_result {
	struct canvas canvas;   /* px malloc'd by layout_render, free it */
	int           dropped;  /* flow lines that did not fit */
	int           cut;      /* lines cut at the right margin */
	int           err_line; /* line of the error, 0 if none */
	char          err[128];
};

/* Run the layout in src (NUL-terminated). Returns 0, or -1 with err and
 * err_line set (and no canvas). */
int layout_render(const char *src, struct layout_result *r);

#endif

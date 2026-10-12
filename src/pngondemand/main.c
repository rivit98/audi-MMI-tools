/*
 * pngondemand - render a text layout to a PNG, on the car.
 *
 *   pngondemand [-s] <layout|-> <out.png|->
 *   pngondemand -h
 *
 * run.sh builds each status screen's layout when it needs it (sd/screens.sh)
 * and shows the PNG with showScreen, so nothing is pre-rendered. Usage and
 * the layout language: README.md here, or -h (the text below).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "img.h"

#define MAX_LAYOUT (256 * 1024)

static const char usage[] =
"usage: pngondemand [-s] <layout|-> <out.png|->\n"
"       pngondemand -h\n"
"Renders a text layout to an 8-bit RGB PNG. '-' = stdin / stdout.\n"
"  -s  strict: text that does not fit is an error (exit 2, PNG still written)\n"
"  -h  this help\n"
"Exit status: 0 ok, 1 usage/layout/I/O error (no PNG), 2 text did not fit (-s).\n";

static const char help[] =
"\n"
"Layout: one command per line, top to bottom. Empty lines and lines starting\n"
"with '#' are ignored. TEXT is the rest of the line after the single space\n"
"that follows the last argument (more spaces indent it). UTF-8.\n"
"\n"
"  size W H                    canvas size, before drawing (default 800 480)\n"
"  colour NAME RRGGBB          name a colour (also: color)\n"
"  bg COLOUR                   fill the canvas (it starts black)\n"
"  rect X Y W H COLOUR         filled rectangle\n"
"  text X Y FONT COLOUR TEXT   text, top-left at X,Y, clipped\n"
"  margin N                    left/right margin of flow text (default 24)\n"
"  at Y                        flow cursor to Y (default 20)\n"
"  gap N                       flow cursor down N px\n"
"  line FONT COLOUR [TEXT]     flow: one line at the margin, cut at the right\n"
"  para FONT COLOUR TEXT       flow: word-wrapped; \"30 s\" stays together\n"
"  footer H FONT BG FG TEXT    bottom bar; flow lines stop above it\n"
"\n"
"FONT: small (8x16) or big (16x32). COLOUR: RRGGBB, #RRGGBB or a colour name.\n"
"Flow lines that would reach the footer or the bottom are dropped.\n"
"\n"
"Example:\n"
"  colour accent 5AC8FA\n"
"  bg 101820\n"
"  rect 0 0 800 6 accent\n"
"  footer 44 small 1C2836 E8E8E8 next app in 8 s\n"
"  line big accent rivit_app\n"
"  para small E8E8E8 Word-wrapped text between the margins.\n";

static char *read_all(const char *path)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
	char *buf;
	size_t n;

	if (f == NULL)
		return NULL;
	buf = malloc(MAX_LAYOUT + 1);
	if (buf == NULL) {
		errno = ENOMEM;
	} else {
		n = fread(buf, 1, MAX_LAYOUT + 1, f);
		if (ferror(f) || n > MAX_LAYOUT || memchr(buf, 0, n) != NULL) {
			if (!ferror(f))
				errno = EFBIG;
			free(buf);
			buf = NULL;
		} else {
			buf[n] = 0;
		}
	}
	if (f != stdin)
		fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	struct layout_result r;
	int strict = 0, rc;
	const char *in, *out;
	char *src;
	FILE *f;

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		fputs(usage, stdout);
		fputs(help, stdout);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "-s") == 0) {
		strict = 1;
		argc--;
		argv++;
	}
	if (argc != 3) {
		fputs(usage, stderr);
		return 1;
	}
	in = argv[1];
	out = argv[2];

	if ((src = read_all(in)) == NULL) {
		fprintf(stderr, "pngondemand: %s: %s\n", in, strerror(errno));
		return 1;
	}
	rc = layout_render(src, &r);
	free(src);
	if (rc != 0) {
		if (r.err_line > 0)
			fprintf(stderr, "pngondemand: %s:%d: %s\n", in, r.err_line, r.err);
		else
			fprintf(stderr, "pngondemand: %s: %s\n", in, r.err);
		return 1;
	}

	f = strcmp(out, "-") == 0 ? stdout : fopen(out, "wb");
	rc = f != NULL ? img_png_write_argb(f, r.canvas.px, r.canvas.stride,
	                                    r.canvas.w, r.canvas.h) : -1;
	if (f != NULL && f != stdout && fclose(f) != 0)
		rc = -1;
	free(r.canvas.px);
	if (rc != 0) {
		fprintf(stderr, "pngondemand: %s: %s\n", out, strerror(errno));
		if (f != stdout)
			remove(out);
		return 1;
	}

	if (r.dropped > 0 || r.cut > 0) {
		fprintf(stderr, "pngondemand: %s: text did not fit: %d line(s) dropped, "
		        "%d cut\n", in, r.dropped, r.cut);
		if (strict)
			return 2;
	}
	return 0;
}

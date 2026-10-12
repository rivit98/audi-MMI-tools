/*
 * text.h - UTF-8 text on a 32-bit canvas with Terminus bitmap fonts.
 *
 * Fonts are generated from PSF console fonts by sdk/mkfont.py (build/gen/
 * font_*.h). Glyph rows are MSB-first bit rows, row_bytes per row. The
 * unicode map is sorted by codepoint; anything not in it (and anything
 * above U+FFFF) is drawn as the font's fallback glyph ('?').
 */
#ifndef USBTX_TEXT_H
#define USBTX_TEXT_H

#include <stdint.h>

struct font {
	int             w, h;
	int             row_bytes;
	const uint8_t  *bits;       /* nglyphs * h * row_bytes */
	const uint16_t *map_cp;     /* sorted codepoints ...         */
	const uint16_t *map_glyph;  /* ... and their glyph indices   */
	int             nmap;
	int             fallback;   /* glyph index for unknown codepoints */
};

/* Pixels are 0xAARRGGBB; stride is in pixels. */
struct canvas {
	uint32_t *px;
	int       w, h, stride;
};

/* Decode one codepoint from *s (stopping at end, which may be NULL for a
 * NUL-terminated string) and advance *s. Returns 0 at the end. Malformed
 * or truncated sequences give U+FFFD and consume one byte. */
uint32_t utf8_next(const char **s, const char *end);

/* Number of codepoints in s[0..nbytes). */
int utf8_count(const char *s, int nbytes);

int font_glyph(const struct font *f, uint32_t cp);

/* Clipped to the canvas. */
void canvas_fill(struct canvas *c, int x, int y, int w, int h, uint32_t color);

/* Draw s[0..nbytes) (nbytes < 0: up to NUL) with its top-left at x,y; only
 * foreground pixels are written. Clipped. Returns the x after the text. */
int text_draw(struct canvas *c, const struct font *f, int x, int y,
              const char *s, int nbytes, uint32_t color);

/* Word-wrap s (NUL-terminated UTF-8) into lines of at most cols
 * codepoints. '\n' ends a line; lines break after the last space that fits,
 * the space itself is dropped; a word longer than cols is split. '\r' is
 * ignored and '\t' counts as a space. cb gets each line (not
 * NUL-terminated). Returns the number of lines (an empty string is one
 * empty line). */
typedef void (*text_line_cb)(const char *line, int nbytes, void *arg);
int text_wrap(const char *s, int cols, text_line_cb cb, void *arg);

#endif

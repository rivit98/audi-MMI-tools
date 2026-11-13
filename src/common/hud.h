/*
 * hud.h - untextured overlay drawn with plain GL quads: filled rectangles
 * and seven-segment numbers (no font texture needed).
 * Coordinates are screen pixels, origin bottom-left (glOrthof(0,w,0,h)).
 */
#ifndef COMMON_HUD_H
#define COMMON_HUD_H

#include "types.h"
#include <GLES/gl.h>

/* Per-vertex colour quad; needs GL_VERTEX_ARRAY + GL_COLOR_ARRAY enabled. */
void hud_rect(f32 x, f32 y, f32 w, f32 h, const GLubyte rgba[4]);

/* Draw value right-aligned in ndigits digit slots starting at (x, y);
 * each digit is digit_w x digit_h pixels. Returns the width of the slots. */
f32 hud_number(f32 x, f32 y, unsigned value, int ndigits,
                 f32 digit_w, f32 digit_h, const GLubyte rgba[4]);

#endif

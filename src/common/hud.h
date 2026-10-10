/*
 * hud.h - untextured overlay drawn with plain GL quads: filled rectangles
 * and seven-segment numbers (no font texture needed).
 * Coordinates are screen pixels, origin bottom-left (glOrthof(0,w,0,h)).
 */
#ifndef RIVIT_HUD_H
#define RIVIT_HUD_H

#include <GLES/gl.h>

/* Per-vertex colour quad; needs GL_VERTEX_ARRAY + GL_COLOR_ARRAY enabled. */
void hud_rect(float x, float y, float w, float h, const GLubyte rgba[4]);

/* Draw value right-aligned in ndigits digit slots starting at (x, y);
 * each digit is digit_w x digit_h pixels. Returns the width of the slots. */
float hud_number(float x, float y, unsigned value, int ndigits,
                 float digit_w, float digit_h, const GLubyte rgba[4]);

#endif

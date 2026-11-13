/*
 * hud.c - rectangles and seven-segment digits (see hud.h).
 */
#include "hud.h"
#include "types.h"

/* segments a..g = bits 0..6 */
static const unsigned char seg_mask[10] = {
	0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f,
};

void hud_rect(f32 x, f32 y, f32 w, f32 h, const GLubyte rgba[4])
{
	const GLfloat v[8] = { x, y,  x + w, y,  x, y + h,  x + w, y + h };
	GLubyte c[16];
	int i;

	for (i = 0; i < 16; i++)
		c[i] = rgba[i & 3];
	glVertexPointer(2, GL_FLOAT, 0, v);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, c);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void digit(f32 x, f32 y, unsigned d, f32 w, f32 h, const GLubyte rgba[4])
{
	f32 t = w / 5.0f, hh = h / 2.0f;
	unsigned m = seg_mask[d % 10];

	if (m & 0x01) hud_rect(x + t,     y + h - t,        w - 2 * t, t,      rgba); /* a */
	if (m & 0x02) hud_rect(x + w - t, y + hh,           t,         hh - t, rgba); /* b */
	if (m & 0x04) hud_rect(x + w - t, y + t,            t,         hh - t, rgba); /* c */
	if (m & 0x08) hud_rect(x + t,     y,                w - 2 * t, t,      rgba); /* d */
	if (m & 0x10) hud_rect(x,         y + t,            t,         hh - t, rgba); /* e */
	if (m & 0x20) hud_rect(x,         y + hh,           t,         hh - t, rgba); /* f */
	if (m & 0x40) hud_rect(x + t,     y + hh - t / 2.0f, w - 2 * t, t,     rgba); /* g */
}

f32 hud_number(f32 x, f32 y, unsigned value, int ndigits,
                 f32 digit_w, f32 digit_h, const GLubyte rgba[4])
{
	f32 step = digit_w * 1.4f;
	int i;

	for (i = ndigits - 1; i >= 0; i--) {
		digit(x + i * step, y, value % 10, digit_w, digit_h, rgba);
		value /= 10;
		if (value == 0 && i > 0)
			break;   /* no leading zeros */
	}
	return ndigits * step;
}

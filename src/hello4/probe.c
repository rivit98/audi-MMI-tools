/*
 * probe.c - pixel-readback checks of the 3D pipeline (see probe.h).
 */
#include "probe.h"

#include <stdlib.h>

#include <GLES/gl.h>

#include "log.h"

#define TOL 40   /* per channel: RGB565 quantisation + lighting/fog rounding */

/* Cleared background: 0.2, 0.4, 0.6 */
#define BG 51, 102, 153

static int w, h, cx, cy;
static int bad_reads;    /* mismatching reads in the current probe */

static void reset(void)
{
	int i;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_LIGHT0);
	glDisable(GL_FOG);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DITHER);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glShadeModel(GL_SMOOTH);
	glEnableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glNormal3f(0.0f, 0.0f, 1.0f);

	glViewport(0, 0, w, h);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0.0f, (GLfloat)w, 0.0f, (GLfloat)h, -1.0f, 1.0f); /* z = +1 nearest */
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
	glClearDepthf(1.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	for (i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++)
		;
}

/* Axis-aligned quad at depth z with texcoords (0,0)-(1,1); counter-
 * clockwise (front-facing by default) unless cw. */
static void quad(float x0, float y0, float x1, float y1, float z, int cw)
{
	GLfloat v[12] = { x0, y0, z,  x1, y0, z,  x0, y1, z,  x1, y1, z };
	static const GLfloat t_ccw[8] = { 0, 0,  1, 0,  0, 1,  1, 1 };
	static const GLfloat t_cw[8]  = { 0, 0,  0, 1,  1, 0,  1, 1 };

	if (cw) {   /* swap the 2nd and 3rd vertex: same strip, other winding */
		v[3] = x0; v[4] = y1;
		v[6] = x1; v[7] = y0;
	}
	glVertexPointer(3, GL_FLOAT, 0, v);
	glTexCoordPointer(2, GL_FLOAT, 0, cw ? t_cw : t_ccw);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void colour(float r, float g, float b)
{
	glColor4f(r, g, b, 1.0f);
}

/* Read pixel (x, y) and compare it with (r, g, b). */
static int expect(int x, int y, int r, int g, int b)
{
	GLubyte p[4] = { 1, 2, 3, 4 };   /* stays if glReadPixels does nothing */
	int ok;

	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	ok = abs(p[0] - r) <= TOL && abs(p[1] - g) <= TOL && abs(p[2] - b) <= TOL;
	log_msg("    (%3d,%3d) got %3u %3u %3u  want %3d %3d %3d  %s",
	        x, y, p[0], p[1], p[2], r, g, b, ok ? "ok" : "BAD");
	if (!ok)
		bad_reads++;
	return ok;
}

static void frustum(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustumf(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 10.0f);  /* ndc x = x / -z */
	glMatrixMode(GL_MODELVIEW);
}

/* ---- probes: each starts from reset() and returns PROBE_* ---- */

static int p_clear(void)
{
	expect(cx, cy, BG);
	expect(0, 0, BG);
	expect(w - 1, h - 1, BG);
	return PROBE_PASS;
}

static int p_depth(void)
{
	glEnable(GL_DEPTH_TEST);
	colour(1, 0, 0);                                        /* near, first */
	quad(cx - 60, cy - 60, cx + 60, cy + 60, 0.5f, 0);
	colour(0, 1, 0);                                        /* far, wider */
	quad(cx - 120, cy - 30, cx + 120, cy + 30, -0.5f, 0);
	expect(cx, cy, 255, 0, 0);
	expect(cx - 90, cy, 0, 255, 0);

	/* blue passes but must not write depth; yellow behind it then wins */
	glDepthMask(GL_FALSE);
	colour(0, 0, 1);
	quad(cx + 70, cy - 20, cx + 110, cy + 20, 0.8f, 0);
	glDepthMask(GL_TRUE);
	colour(1, 1, 0);
	quad(cx + 70, cy - 20, cx + 110, cy + 20, 0.6f, 0);
	expect(cx + 90, cy, 255, 255, 0);
	return PROBE_PASS;
}

static int p_cull(void)
{
	glEnable(GL_CULL_FACE);
	colour(1, 0, 0);
	quad(w / 8, h / 4, 3 * w / 8, 3 * h / 4, 0, 1);         /* CW: culled */
	colour(0, 1, 0);
	quad(5 * w / 8, h / 4, 7 * w / 8, 3 * h / 4, 0, 0);     /* CCW: drawn */
	expect(w / 4, h / 2, BG);
	expect(3 * w / 4, h / 2, 0, 255, 0);

	glFrontFace(GL_CW);
	colour(0, 0, 1);
	quad(cx - 20, h / 4, cx + 20, 3 * h / 4, 0, 1);         /* CW now front */
	colour(1, 0, 0);
	quad(cx - 20, h / 8, cx + 20, h / 5, 0, 0);             /* CCW now back */
	expect(cx, h / 2, 0, 0, 255);
	expect(cx, h / 6, BG);
	return PROBE_PASS;
}

static int p_persp(void)
{
	frustum();
	glEnable(GL_DEPTH_TEST);
	colour(1, 0, 0);
	quad(-1, -1, 1, 1, -2.0f, 0);       /* ndc +-0.5 */
	colour(0, 1, 0);
	quad(-3, -3, 3, 3, -4.0f, 0);       /* ndc +-0.75, behind red */
	expect(cx, cy, 255, 0, 0);
	expect(w / 5, cy, 0, 255, 0);       /* ndc -0.6 */
	expect(cx, h / 5, 0, 255, 0);
	expect(w / 20, cy, BG);             /* ndc -0.9 */
	expect(cx, 19 * h / 20, BG);
	return PROBE_PASS;
}

static int p_light(void)
{
	static const GLfloat zero[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	static const GLfloat red[4] = { 1, 0, 0, 1 }, towards_viewer[4] = { 0, 0, 1, 0 };

	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
	glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
	glLightfv(GL_LIGHT0, GL_SPECULAR, zero);
	glLightfv(GL_LIGHT0, GL_POSITION, towards_viewer);   /* directional */
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
	glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
	glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);

	glNormal3f(0.0f, 0.0f, 1.0f);                         /* N.L = 1 */
	quad(0, h / 4, w / 3, 3 * h / 4, 0, 0);
	glNormal3f(0.0f, 0.70710678f, 0.70710678f);           /* N.L = 0.707 */
	quad(w / 3, h / 4, 2 * w / 3, 3 * h / 4, 0, 0);
	glNormal3f(0.0f, 0.0f, -1.0f);                        /* N.L < 0 */
	quad(2 * w / 3, h / 4, w, 3 * h / 4, 0, 0);
	expect(w / 6, cy, 255, 0, 0);
	expect(cx, cy, 180, 0, 0);
	expect(5 * w / 6, cy, 0, 0, 0);
	return PROBE_PASS;
}

static int p_texture(void)
{
	/* bottom row first: red green / blue white */
	static const GLubyte texels[16] = {
		255, 0, 0, 255,   0, 255, 0, 255,
		0, 0, 255, 255,   255, 255, 255, 255,
	};
	GLuint t = 0;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glEnable(GL_TEXTURE_2D);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	quad(w / 4, h / 4, 3 * w / 4, 3 * h / 4, 0, 0);
	expect(3 * w / 8, 3 * h / 8, 255, 0, 0);
	expect(5 * w / 8, 3 * h / 8, 0, 255, 0);
	expect(3 * w / 8, 5 * h / 8, 0, 0, 255);
	expect(5 * w / 8, 5 * h / 8, 255, 255, 255);
	glDisable(GL_TEXTURE_2D);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDeleteTextures(1, &t);
	return PROBE_PASS;
}

static int p_fog(void)
{
	static const GLfloat yellow[4] = { 1, 1, 0, 1 };
	static const float range[3][2] = { { 1, 3 }, { 3, 7 }, { 6, 9 } };
	static const int want[3][3] = { { 255, 255, 0 }, { 127, 127, 127 }, { 0, 0, 255 } };
	int i;

	frustum();
	glEnable(GL_FOG);
	glFogf(GL_FOG_MODE, GL_LINEAR);
	glFogfv(GL_FOG_COLOR, yellow);
	/* blue quad 5 units away: past the fog end, halfway, before the start */
	for (i = 0; i < 3; i++) {
		glFogf(GL_FOG_START, range[i][0]);
		glFogf(GL_FOG_END, range[i][1]);
		glClear(GL_COLOR_BUFFER_BIT);
		colour(0, 0, 1);
		quad(-1, -1, 1, 1, -5.0f, 0);
		expect(cx, cy, want[i][0], want[i][1], want[i][2]);
	}
	return PROBE_PASS;
}

static int p_blend(void)
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
	quad(w / 4, h / 4, 3 * w / 4, 3 * h / 4, 0, 0);
	expect(cx, cy, 153, 178, 204);     /* halfway from BG to white */
	expect(w / 8, cy, BG);
	return PROBE_PASS;
}

static int p_stencil(void)
{
	GLint bits = 0;

	glGetIntegerv(GL_STENCIL_BITS, &bits);
	if (bits <= 0) {
		log_msg("    no stencil bits in this EGL config");
		return PROBE_SKIP;
	}
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	quad(cx - 40, cy - 40, cx + 40, cy + 40, 0, 0);      /* mask only */
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	colour(1, 0, 0);
	quad(0, 0, w, h, 0, 0);
	expect(cx, cy, 255, 0, 0);
	expect(cx - 80, cy, BG);
	return PROBE_PASS;
}

static const struct {
	const char *name;
	int (*run)(void);
} probes[] = {
	{ "clear", p_clear },
	{ "depth", p_depth },
	{ "cull", p_cull },
	{ "persp", p_persp },
	{ "light", p_light },
	{ "texture", p_texture },
	{ "fog", p_fog },
	{ "blend", p_blend },
	{ "stencil", p_stencil },
};
#define NPROBES ((int)(sizeof(probes) / sizeof(probes[0])))

int probes_run(int width, int height, unsigned char state[PROBE_MAX])
{
	static const char *const verdict[] = { "SKIP", "PASS", "FAIL" };
	int i, n[3] = { 0, 0, 0 };

	w = width;
	h = height;
	cx = w / 2;
	cy = h / 2;
	log_msg("probes: %d checks by glReadPixels, tolerance %d per channel", NPROBES, TOL);
	for (i = 0; i < NPROBES && i < PROBE_MAX; i++) {
		GLenum err;
		int s;

		reset();
		bad_reads = 0;
		log_msg("  probe %s", probes[i].name);
		s = probes[i].run();
		err = glGetError();
		if (s == PROBE_PASS && (bad_reads || err != GL_NO_ERROR))
			s = PROBE_FAIL;
		state[i] = s;
		n[s]++;
		log_msg("  probe %-8s %s  (bad reads %d, glError=0x%04x)",
		        probes[i].name, verdict[s], bad_reads, err);
	}
	reset();
	log_msg("probes: %d pass, %d fail, %d skip", n[PROBE_PASS], n[PROBE_FAIL], n[PROBE_SKIP]);
	return i;
}

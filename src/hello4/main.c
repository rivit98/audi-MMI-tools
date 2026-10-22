/*
 * hello4 - 3D pipeline test for the MMI 3G+ (follow-up to hello3).
 *
 * hello3 drew flat textured quads. hello4 uses the 3D half of the unit's
 * fixed-function ES-CM 1.0 (NVIDIA): perspective projection, depth buffer,
 * back-face culling, indexed meshes, per-vertex lighting with specular,
 * mipmapped textures and fog.
 *
 * It starts with automatic probes (probe.c): small test renders read back
 * with glReadPixels. Each one logs PASS/FAIL/SKIP, and the results stay
 * on screen as a row of squares, top right: green = pass, red = fail,
 * grey = skip. Then four scenes of <scene_seconds> each (default 10 s):
 *
 *   1. cube    two vertex-coloured cubes, the small one orbiting through
 *              the big one: depth buffer, culling, Gouraud colour, matrix
 *              stack (glPush/PopMatrix), glDrawElements.
 *   2. light   gold torus lit by a white directional light and an
 *              orbiting blue point light (shown as a small cube), with
 *              specular highlights and attenuation. Smooth shading for the
 *              first half, GL_FLAT for the second.
 *   3. world   mipmapped checkerboard ground fading into fog, a ring of
 *              textured, lit crates and a floating torus, with the camera
 *              circling: textures x lighting x fog together.
 *   4. stress  1, 2, 4, 8, 12 lit tori (1024 triangles each). Logs fps
 *              and triangles/s per step. 16 and up are commented out
 *              (opts_default): 16 froze the unit on 2026-10-19.
 * Top left: frames in the previous second (green) and, in the stress
 * scene, the torus count (yellow). Bottom: progress bar of the current
 * scene (of the current step, in stress).
 *
 * The 2026-10-11 car run passed every probe and scenes 1-3, then froze
 * the whole unit (picture, knobs, radio volume; no watchdog fired) about
 * 5.5 s into the stress scene, while it was drawing 8 tori
 * (docs/gpu-results.md). So the scenes and the stress steps are
 * selectable, and the stress scene can log every frame (trace=), so the
 * last fsync'ed line on the card says which frame the unit died in.
 * 2026-10-19 (stress only): 8 tori for 60 s and 12 passed, the first
 * 16-tori frame froze the unit. The default plan now stops at 12.
 *
 * Stops early, with the normal teardown, on SIGTERM/SIGINT/SIGHUP, SD
 * card removal or the watchdog (src/common/quit.c). A freeze of the whole
 * unit stops nothing: only the MMI reboot key combo helps then.
 *
 * Output on the card: hello4.log (GL limits, probe reads + verdicts,
 * per-second fps + free RAM, triangle-rate table) and layer manager
 * screenshots of scenes 1-3.
 *
 * usage: hello4 <sd_path> [scene_seconds] [option=value ...]
 *   scene_seconds  length of scenes cube/light/world, and of the whole
 *                  stress scene when step= is not given (default 10)
 *   scenes=pclws   what runs, in this order: p = probes, c = cube,
 *                  l = light, w = world, s = stress (default: all)
 *   tori=1,2,8:60  the stress steps: torus counts, each optionally with
 *                  its own seconds (default 1,2,4,8,12)
 *   step=S         seconds of a stress step without its own (default:
 *                  scene_seconds split evenly over the steps)
 *   trace=0|1|2    stress scene only. 1: a line per frame after its swap
 *                  (frame, tori, draw ms, swap ms); 2: also a line
 *                  before drawing and before swapping. Every line is
 *                  fsync'ed (~12 ms on the card), which lowers the fps.
 * e.g. hello4 /mnt/sdcard10t12 scenes=s step=5 tori=1,2,4,8:60,12 trace=1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <GLES/gl.h>

#include "gfx.h"
#include "hud.h"
#include "log.h"
#include "mesh.h"
#include "probe.h"
#include "quit.h"

#define CID             40     /* displayable id qnx-audi uses */
#define SCENE_SECONDS   10
#define WATCHDOG_SLACK  40     /* stop (then _exit) if GL or the LM hangs */
#define TEX_SIZE        64     /* power of two (GLES 1.0) */
#define MAX_STEPS       16     /* stress steps */
#define MAX_TORI        256

enum { SCENE_CUBE, SCENE_LIGHT, SCENE_WORLD, SCENE_STRESS, NSCENES };
static const char *const scene_name[NSCENES] = { "cube", "light", "world", "stress" };
static const char scene_letter[NSCENES] = { 'c', 'l', 'w', 's' };

/* Command line, see the top of the file. */
struct opts {
	unsigned scene_ms;
	int probes;
	unsigned nscenes;
	int scene[NSCENES];          /* in run order */
	unsigned nsteps;
	unsigned tori[MAX_STEPS];
	unsigned tori_ms[MAX_STEPS]; /* 0: step_ms */
	unsigned step_ms;            /* 0: scene_ms / nsteps */
	int trace;
};

/* One stretch of the run: a whole scene, or one stress step. */
struct seg {
	int scene;
	unsigned ms;
	unsigned step;               /* stress: index into opts.tori */
};
#define MAX_SEGS (NSCENES - 1 + MAX_STEPS)

/* clear colours */
static const GLfloat sky[4]   = { 0.55f, 0.70f, 0.90f, 1.0f };   /* = fog colour */
static const GLfloat navy[4]  = { 0.05f, 0.07f, 0.18f, 1.0f };
static const GLfloat black[4] = { 0.04f, 0.04f, 0.06f, 1.0f };
static const GLfloat grey[4]  = { 0.10f, 0.10f, 0.12f, 1.0f };

static const char *sd_path;
static struct mesh cube, torus, ground;
static GLuint checker_tex, crate_tex;
static unsigned char probe_state[PROBE_MAX];
static int nprobes;

/* ---- assets (built once by the CPU) ---- */

/* 2x2 box filter, size -> size/2, in place: every output pixel lies
 * before all the input pixels still to be read. */
static void halve(GLubyte *p, int size)
{
	int x, y, c, half = size / 2;

	for (y = 0; y < half; y++)
		for (x = 0; x < half; x++)
			for (c = 0; c < 4; c++) {
				const GLubyte *s = p + ((2 * y) * size + 2 * x) * 4 + c;

				p[(y * half + x) * 4 + c] =
					(s[0] + s[4] + s[size * 4] + s[size * 4 + 4] + 2) / 4;
			}
}

/* Upload with the full mip chain (GLES 1.0 has no GL_GENERATE_MIPMAP),
 * trilinear filtered. rgba is overwritten. */
static GLuint upload_mipmapped(GLubyte *rgba, int size)
{
	GLuint t = 0;
	int level;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	for (level = 0; ; level++, size /= 2) {
		glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, size, size, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		if (size == 1)
			break;
		halve(rgba, size);
	}
	return t;
}

static void fill_checker(GLubyte *p, int size)
{
	int x, y;

	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++, p += 4) {
			int light = ((x >> 3) ^ (y >> 3)) & 1;

			p[0] = light ? 225 : 60;
			p[1] = light ? 225 : 120;
			p[2] = light ? 210 : 60;
			p[3] = 255;
		}
}

/* Wooden crate: dark frame, planks with seams, a diagonal brace. */
static void fill_crate(GLubyte *p, int size)
{
	int x, y;

	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++, p += 4) {
			int grain = ((x * 7) ^ (y * 13)) & 15;
			int frame = x < 5 || y < 5 || x >= size - 5 || y >= size - 5;
			int dark = abs(x - y) < 4 || y % 16 == 0;
			int k = frame ? 50 : dark ? 75 : 100;    /* percent of the base */

			p[0] = (176 + grain) * k / 100;
			p[1] = (124 + grain) * k / 100;
			p[2] = (64 + grain / 2) * k / 100;
			p[3] = 255;
		}
}

static int make_assets(void)
{
	GLubyte *buf = malloc(TEX_SIZE * TEX_SIZE * 4);
	unsigned t0 = log_ms();

	if (buf == NULL || mesh_cube(&cube) || mesh_torus(&torus, 1.0f, 0.4f, 32, 16) ||
	    mesh_grid(&ground, 60.0f, 24, 20.0f)) {
		log_msg("FAIL out of memory building meshes/textures");
		free(buf);
		return -1;
	}
	fill_checker(buf, TEX_SIZE);
	checker_tex = upload_mipmapped(buf, TEX_SIZE);
	fill_crate(buf, TEX_SIZE);
	crate_tex = upload_mipmapped(buf, TEX_SIZE);
	free(buf);
	log_msg("meshes: cube %d tris, torus %d tris (%d verts), ground %d tris",
	        cube.nidx / 3, torus.nidx / 3, torus.nverts, ground.nidx / 3);
	log_msg("textures: checker id %u, crate id %u, %dx%d + mipmaps, in %u ms, glError=0x%04x",
	        checker_tex, crate_tex, TEX_SIZE, TEX_SIZE, log_ms() - t0, glGetError());
	return checker_tex && crate_tex ? 0 : -1;
}

static void free_assets(void)
{
	mesh_free(&cube);
	mesh_free(&torus);
	mesh_free(&ground);
}

/* ---- GL state ---- */

/* Light colours, fog and hints; positions are set per frame, in world
 * space, after the camera transform. */
static void setup_lights_fog(void)
{
	static const GLfloat ambient[4] = { 0.18f, 0.18f, 0.22f, 1.0f };
	static const GLfloat none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	static const GLfloat sun[4] = { 0.85f, 0.82f, 0.75f, 1.0f };
	static const GLfloat white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat lamp[4] = { 0.3f, 0.6f, 1.0f, 1.0f };

	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
	glLightfv(GL_LIGHT0, GL_AMBIENT, none);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, sun);
	glLightfv(GL_LIGHT0, GL_SPECULAR, white);
	glLightfv(GL_LIGHT1, GL_AMBIENT, none);
	glLightfv(GL_LIGHT1, GL_DIFFUSE, lamp);
	glLightfv(GL_LIGHT1, GL_SPECULAR, lamp);
	glLightf(GL_LIGHT1, GL_LINEAR_ATTENUATION, 0.15f);
	glEnable(GL_NORMALIZE);          /* the stress tori are scaled */

	gfx_fog_linear(10.0f, 32.0f);
	glFogfv(GL_FOG_COLOR, sky);

	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glHint(GL_FOG_HINT, GL_NICEST);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	log_msg("lights/fog set up, glError=0x%04x", glGetError());
}

static void material(float r, float g, float b, float spec, float shininess)
{
	const GLfloat colour[4] = { r, g, b, 1.0f }, sp[4] = { spec, spec, spec, 1.0f };

	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, colour);
	glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, sp);
	glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, shininess);
}

/* 60 degree vertical field of view; clears colour + depth. */
static void begin_3d(int w, int h, const GLfloat clear[4])
{
	const float znear = 0.5f, zfar = 60.0f, top = znear * 0.57735027f;
	const float right = top * w / h;

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustumf(-right, right, -top, top, znear, zfar);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glEnable(GL_CULL_FACE);
	glShadeModel(GL_SMOOTH);
	glClearColor(clear[0], clear[1], clear[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* ---- scenes ---- */

static void draw_cube_scene(float t)
{
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glTranslatef(0.0f, 0.0f, -6.0f);

	glPushMatrix();
	glRotatef(t * 40.0f, 1.0f, 0.0f, 0.0f);
	glRotatef(t * 65.0f, 0.0f, 1.0f, 0.0f);
	mesh_draw(&cube, MESH_COLOR);
	glPopMatrix();

	/* orbit radius 1.4 < big cube's corner (1.73): they intersect */
	glPushMatrix();
	glRotatef(t * 90.0f, 0.2f, 1.0f, 0.0f);
	glTranslatef(1.4f, 0.0f, 0.0f);
	glRotatef(t * 150.0f, 1.0f, 1.0f, 0.0f);
	glScalef(0.6f, 0.6f, 0.6f);
	mesh_draw(&cube, MESH_COLOR);
	glPopMatrix();
}

static void draw_light_scene(float t, int flat)
{
	static const GLfloat sun_dir[4] = { -0.5f, 0.6f, 0.8f, 0.0f };
	const GLfloat lamp[4] = { 2.2f * cosf(t * 1.3f), sinf(t * 0.7f), 2.2f * sinf(t * 1.3f), 1.0f };

	glDisable(GL_TEXTURE_2D);
	glTranslatef(0.0f, 0.0f, -5.5f);
	glLightfv(GL_LIGHT0, GL_POSITION, sun_dir);
	glLightfv(GL_LIGHT1, GL_POSITION, lamp);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_LIGHT1);
	glShadeModel(flat ? GL_FLAT : GL_SMOOTH);

	material(0.9f, 0.65f, 0.2f, 0.9f, 40.0f);
	glPushMatrix();
	glRotatef(t * 30.0f, 1.0f, 0.0f, 0.0f);
	glRotatef(t * 50.0f, 0.0f, 1.0f, 0.0f);
	mesh_draw(&torus, MESH_NORMAL);
	glPopMatrix();

	/* the point light itself, unlit */
	glDisable(GL_LIGHTING);
	glColor4f(0.4f, 0.7f, 1.0f, 1.0f);
	glPushMatrix();
	glTranslatef(lamp[0], lamp[1], lamp[2]);
	glScalef(0.1f, 0.1f, 0.1f);
	mesh_draw(&cube, 0);
	glPopMatrix();
	glDisable(GL_LIGHT1);
}

static void draw_world_scene(float t)
{
	static const GLfloat sun_dir[4] = { 0.4f, 1.0f, 0.3f, 0.0f };
	int i;

	/* camera 12 units out, 22 degrees up, circling the origin */
	glTranslatef(0.0f, 0.0f, -12.0f);
	glRotatef(22.0f, 1.0f, 0.0f, 0.0f);
	glRotatef(t * 18.0f, 0.0f, 1.0f, 0.0f);
	glLightfv(GL_LIGHT0, GL_POSITION, sun_dir);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_FOG);

	glEnable(GL_TEXTURE_2D);
	material(1.0f, 1.0f, 1.0f, 0.0f, 1.0f);
	glBindTexture(GL_TEXTURE_2D, checker_tex);
	mesh_draw(&ground, MESH_NORMAL | MESH_UV);

	glBindTexture(GL_TEXTURE_2D, crate_tex);
	material(1.0f, 1.0f, 1.0f, 0.3f, 20.0f);
	for (i = 0; i < 8; i++) {
		glPushMatrix();
		glRotatef(i * 45.0f, 0.0f, 1.0f, 0.0f);
		glTranslatef(5.0f, 1.0f, 0.0f);          /* resting on the ground */
		glRotatef(t * 40.0f + i * 30.0f, 0.0f, 1.0f, 0.0f);
		mesh_draw(&cube, MESH_NORMAL | MESH_UV);
		glPopMatrix();
	}

	glDisable(GL_TEXTURE_2D);
	material(0.9f, 0.65f, 0.2f, 0.9f, 40.0f);
	glPushMatrix();
	glTranslatef(0.0f, 2.2f + 0.4f * sinf(t * 2.0f), 0.0f);
	glRotatef(t * 70.0f, 1.0f, 0.3f, 0.0f);
	mesh_draw(&torus, MESH_NORMAL);
	glPopMatrix();
	glDisable(GL_FOG);
}

static void draw_stress_scene(float t, unsigned n)
{
	static const GLfloat sun_dir[4] = { -0.5f, 0.6f, 0.8f, 0.0f };
	static const GLfloat hue[4][3] = {
		{ 0.9f, 0.65f, 0.2f }, { 0.3f, 0.7f, 0.9f },
		{ 0.8f, 0.3f, 0.5f }, { 0.4f, 0.85f, 0.4f },
	};
	unsigned cols = 1, rows, i;
	float sp;

	/* grid roughly the screen's 5:3 shape; the view is ~15 x 9 units at z=-8 */
	while (cols * cols < n * 5 / 3)
		cols++;
	rows = (n + cols - 1) / cols;
	sp = 14.0f / cols < 8.5f / rows ? 14.0f / cols : 8.5f / rows;
	if (sp > 4.0f)
		sp = 4.0f;

	glDisable(GL_TEXTURE_2D);
	glTranslatef(0.0f, 0.0f, -8.0f);
	glLightfv(GL_LIGHT0, GL_POSITION, sun_dir);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	for (i = 0; i < n; i++) {
		const float *c = hue[i % 4];

		material(c[0], c[1], c[2], 0.7f, 30.0f);
		glPushMatrix();
		glTranslatef((i % cols - (cols - 1) * 0.5f) * sp,
		             ((rows - 1) * 0.5f - i / cols) * sp, 0.0f);
		glRotatef(t * 60.0f + i * 23.0f, 1.0f, 0.5f, 0.0f);
		glScalef(sp * 0.32f, sp * 0.32f, sp * 0.32f);
		mesh_draw(&torus, MESH_NORMAL);
		glPopMatrix();
	}
}

/* ---- HUD ---- */

static void draw_hud(int w, int h, unsigned fps, unsigned count, float progress)
{
	static const GLubyte backing[4] = { 0, 0, 0, 170 };
	static const GLubyte green[4] = { 80, 255, 80, 255 };
	static const GLubyte yellow[4] = { 255, 200, 0, 255 };
	static const GLubyte state_col[3][4] = {
		{ 130, 130, 130, 255 }, { 60, 220, 60, 255 }, { 235, 40, 40, 255 },
	};
	float px = w - 8.0f - nprobes * 20.0f;
	int i;

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0.0f, (GLfloat)w, 0.0f, (GLfloat)h, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_FOG);
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	hud_rect(8.0f, h - 56.0f, count ? 210.0f : 110.0f, 48.0f, backing);
	if (nprobes)
		hud_rect(px - 6.0f, h - 34.0f, nprobes * 20.0f + 8.0f, 28.0f, backing);
	glDisable(GL_BLEND);
	hud_number(18.0f, h - 48.0f, fps, 3, 18.0f, 32.0f, green);
	if (count)
		hud_number(118.0f, h - 48.0f, count, 3, 18.0f, 32.0f, yellow);
	for (i = 0; i < nprobes; i++)
		hud_rect(px + i * 20.0f, h - 28.0f, 16.0f, 16.0f, state_col[probe_state[i]]);
	hud_rect(0.0f, 0.0f, w * progress, 8.0f, yellow);
}

/* ---- main loop ---- */

static void screenshot(const char *scene)
{
	char cmd[320];
	int rc;

	snprintf(cmd, sizeof(cmd), "s-shot 0 %s/hello4_%s.png", sd_path, scene);
	rc = lm_text_command(cmd);
	log_msg("layermanager '%s': %s", cmd, rc ? strerror(rc) : "ok");
}

/* Free system RAM in MB (on QNX the size of /proc is the free memory),
 * logged to catch a leak in the driver before a hang. 0 if unknown. */
static unsigned free_mb(void)
{
	struct stat st;

	if (stat("/proc", &st) != 0)
		return 0;
	return (unsigned)((unsigned long long)st.st_size >> 20);
}

static unsigned build_segs(const struct opts *o, struct seg *seg)
{
	unsigned n = 0, i, s;

	for (i = 0; i < o->nscenes; i++) {
		if (o->scene[i] != SCENE_STRESS) {
			seg[n].scene = o->scene[i];
			seg[n].ms = o->scene_ms;
			seg[n].step = 0;
			n++;
			continue;
		}
		for (s = 0; s < o->nsteps; s++) {
			seg[n].scene = SCENE_STRESS;
			seg[n].step = s;
			seg[n].ms = o->tori_ms[s] ? o->tori_ms[s] :
			            o->step_ms ? o->step_ms : o->scene_ms / o->nsteps;
			if (seg[n].ms == 0)
				seg[n].ms = 1;   /* the progress bar divides by it */
			n++;
		}
	}
	return n;
}

static void seg_label(const struct opts *o, const struct seg *s, char *buf, size_t n)
{
	if (s->scene == SCENE_STRESS)
		snprintf(buf, n, "stress step %u/%u (%u tori)", s->step + 1, o->nsteps,
		         o->tori[s->step]);
	else
		snprintf(buf, n, "scene %s", scene_name[s->scene]);
}

static void log_seg_start(const struct opts *o, const struct seg *s)
{
	char label[64];

	seg_label(o, s, label, sizeof(label));
	if (s->scene == SCENE_STRESS)
		log_msg("%s: start, %u ms, %u tris/frame, free RAM %u MB", label, s->ms,
		        o->tori[s->step] * (unsigned)(torus.nidx / 3), free_mb());
	else
		log_msg("%s: start, %u ms", label, s->ms);
}

static void log_seg_done(const struct opts *o, const struct seg *s, unsigned frames,
                         unsigned ms)
{
	char label[64];

	seg_label(o, s, label, sizeof(label));
	log_msg("%s done: %u frames in %u ms (avg %u fps)", label, frames, ms,
	        frames * 1000 / (ms ? ms : 1));
}

struct stress_stat {
	unsigned frames, first_ms, last_ms;
};

static void log_stress_table(const struct opts *o, const struct stress_stat *st)
{
	unsigned i, tris = torus.nidx / 3;

	for (i = 0; i < o->nsteps && st[i].frames == 0; i++)
		;
	if (i == o->nsteps)
		return;          /* the stress scene did not run */
	log_msg("triangle-rate results (lit, smooth, untextured tori, %u tris each):", tris);
	for (i = 0; i < o->nsteps; i++) {
		unsigned span = st[i].last_ms - st[i].first_ms, n = o->tori[i];

		if (st[i].frames == 0)
			continue;
		/* the first frame of a step is the warm-up: its end starts the clock */
		if (st[i].frames < 2 || span == 0) {
			log_msg("  %2u tori: too few frames (%u)", n, st[i].frames);
			continue;
		}
		log_msg("  %2u tori = %5u tris: %3u frames in %4u ms = %3u.%u fps = %5u ktris/s",
		        n, n * tris, st[i].frames - 1, span,
		        (st[i].frames - 1) * 1000 / span, (st[i].frames - 1) * 10000 / span % 10,
		        (unsigned)((unsigned long long)(st[i].frames - 1) * n * tris / span));
	}
}

static void run(struct gfx *g, const struct opts *o)
{
	struct seg seg[MAX_SEGS];
	unsigned nseg = build_segs(o, seg), run_ms = 0, i;
	unsigned start, last, next_report = 1000;
	unsigned frames = 0, sec_frames = 0, fps_shown = 0, worst = 0, swap_fail = 0;
	unsigned cur = 0, seg_start = 0, scene_start = 0, seg_frames = 0, shot_done = 0;
	/* Milliseconds spent on screenshots. Kept out of the scene clock and of
	 * the fps / worst-frame numbers: capturing is not drawing. */
	unsigned paused = 0;
	struct stress_stat stress[MAX_STEPS];
	int w = g->width, h = g->height;

	for (i = 0; i < nseg; i++)
		run_ms += seg[i].ms;
	memset(stress, 0, sizeof(stress));
	glViewport(0, 0, w, h);
	glDisable(GL_DITHER);
	log_msg("run: %u part(s), %u ms, trace=%d, free RAM %u MB, glError=0x%04x",
	        nseg, run_ms, o->trace, free_mb(), glGetError());
	if (nseg == 0)
		return;
	start = last = log_ms();
	log_seg_start(o, &seg[0]);

	for (;;) {
		unsigned now = log_ms(), ms = now - start - paused, sms, count = 0, t0, t_draw, t_swap;
		const struct seg *sg;
		float t;

		if (ms >= run_ms)
			quit_request(QUIT_TIMER);
		if (quit_poll() != QUIT_NONE)
			break;
		while (cur + 1 < nseg && ms >= seg_start + seg[cur].ms) {
			log_seg_done(o, &seg[cur], seg_frames, ms - seg_start);
			seg_start += seg[cur].ms;
			cur++;
			if (seg[cur].scene != seg[cur - 1].scene)
				scene_start = seg_start;
			seg_frames = 0;
			shot_done = 0;
			log_seg_start(o, &seg[cur]);
		}
		sg = &seg[cur];
		sms = ms - seg_start;
		t = (ms - scene_start) / 1000.0f;
		if (sg->scene == SCENE_STRESS) {
			count = o->tori[sg->step];
			if (o->trace >= 2)
				log_msg("f%u draw", frames);
		}

		t0 = log_ms();
		switch (sg->scene) {
		case SCENE_CUBE:
			begin_3d(w, h, navy);
			draw_cube_scene(t);
			break;
		case SCENE_LIGHT:
			begin_3d(w, h, black);
			draw_light_scene(t, sms >= sg->ms / 2);
			break;
		case SCENE_WORLD:
			begin_3d(w, h, sky);
			draw_world_scene(t);
			break;
		default:
			begin_3d(w, h, grey);
			draw_stress_scene(t, count);
			break;
		}
		draw_hud(w, h, fps_shown, count, (float)sms / sg->ms);
		t_draw = log_ms() - t0;

		/* screenshot mid-scene; not in the stress scene, it would skew timing */
		if (!shot_done && sg->scene != SCENE_STRESS && sms >= sg->ms / 2) {
			t0 = log_ms();
			screenshot(scene_name[sg->scene]);
			paused += log_ms() - t0;
			last = log_ms();        /* don't bill the capture to this frame */
			shot_done = 1;
		}
		if (count && o->trace >= 2)
			log_msg("f%u swap", frames);
		t0 = log_ms();
		if (gfx_swap(g) != 0 && swap_fail++ < 3)
			log_msg("eglSwapBuffers failed: %s", egl_err_name(eglGetError()));
		now = log_ms();
		t_swap = now - t0;
		/* the last line on the card names the last frame that completed */
		if (count && o->trace >= 1)
			log_msg("f%u n=%u draw=%u swap=%u", frames, count, t_draw, t_swap);

		frames++;
		sec_frames++;
		seg_frames++;
		if (sg->scene == SCENE_STRESS) {
			struct stress_stat *st = &stress[sg->step];

			if (st->frames++ == 0)
				st->first_ms = now;
			st->last_ms = now;
		}
		if (now - last > worst)
			worst = now - last;
		last = now;
		if (ms >= next_report) {
			log_msg("t=%2u s  %-6s fps=%u  worst frame=%u ms  tori=%u  free=%u MB  glError=0x%04x",
			        ms / 1000, scene_name[sg->scene], sec_frames, worst, count, free_mb(),
			        glGetError());
			fps_shown = sec_frames;
			sec_frames = 0;
			worst = 0;
			next_report += 1000;
		}
	}
	log_seg_done(o, &seg[cur], seg_frames, log_ms() - start - paused - seg_start);
	log_stress_table(o, stress);
	run_ms = log_ms() - start - paused;   /* actual, shorter after an early stop */
	if (run_ms == 0)
		run_ms = 1;
	log_msg("run done: %u frames in %u ms (avg %u.%u fps), swap failures=%u",
	        frames, run_ms, frames * 1000 / run_ms, frames * 10000 / run_ms % 10, swap_fail);
	if (paused != 0)
		log_msg("  (+%u ms capturing and writing files, not counted above)", paused);
}

/* ---- command line ---- */

static void opts_default(struct opts *o)
{
	/* The first 16-tori frame froze the whole unit on 2026-10-19
	 * (docs/gpu-results.md section 8), and more tori only load the GPU
	 * harder, so 16 and up are out. tori= can still ask for them. */
	static const unsigned tori[] = {
		1, 2, 4, 8, 12,
		/* 16, 32, 64, */
	};
	unsigned i;

	memset(o, 0, sizeof(*o));
	o->scene_ms = SCENE_SECONDS * 1000;
	o->probes = 1;
	for (i = 0; i < NSCENES; i++)
		o->scene[o->nscenes++] = i;
	for (i = 0; i < sizeof(tori) / sizeof(tori[0]); i++)
		o->tori[o->nsteps++] = tori[i];
}

/* "pclws": each letter at most once */
static int parse_scenes(struct opts *o, const char *s)
{
	unsigned seen = 0;
	int i;

	o->probes = 0;
	o->nscenes = 0;
	for (; *s != '\0'; s++) {
		if (*s == 'p') {
			o->probes = 1;
			continue;
		}
		for (i = 0; i < NSCENES && scene_letter[i] != *s; i++)
			;
		if (i == NSCENES || (seen & (1u << i)))
			return -1;
		seen |= 1u << i;
		o->scene[o->nscenes++] = i;
	}
	return 0;
}

/* "1,2,8:60,16": torus counts, each with optional seconds */
static int parse_tori(struct opts *o, const char *s)
{
	o->nsteps = 0;
	while (*s != '\0') {
		char *end;
		unsigned long n = strtoul(s, &end, 10), secs = 0;

		if (end == s || n == 0 || n > MAX_TORI || o->nsteps == MAX_STEPS)
			return -1;
		if (*end == ':') {
			s = end + 1;
			secs = strtoul(s, &end, 10);
			if (end == s || secs == 0)
				return -1;
		}
		o->tori[o->nsteps] = (unsigned)n;
		o->tori_ms[o->nsteps] = (unsigned)secs * 1000;
		o->nsteps++;
		if (*end == ',')
			end++;
		else if (*end != '\0')
			return -1;
		s = end;
	}
	return o->nsteps ? 0 : -1;
}

/* argv[2..]; every bad argument is logged as a FAIL line */
static int parse_args(struct opts *o, int argc, char *argv[])
{
	int i, bad = 0;

	opts_default(o);
	for (i = 2; i < argc; i++) {
		const char *a = argv[i];
		int ok;

		if (a[0] >= '0' && a[0] <= '9')
			ok = (o->scene_ms = (unsigned)atoi(a) * 1000) != 0;
		else if (strncmp(a, "scenes=", 7) == 0)
			ok = parse_scenes(o, a + 7) == 0;
		else if (strncmp(a, "tori=", 5) == 0)
			ok = parse_tori(o, a + 5) == 0;
		else if (strncmp(a, "step=", 5) == 0)
			ok = (o->step_ms = (unsigned)atoi(a + 5) * 1000) != 0;
		else if (strncmp(a, "trace=", 6) == 0)
			ok = (o->trace = atoi(a + 6)) >= 0 && o->trace <= 2;
		else
			ok = 0;
		if (!ok) {
			log_msg("FAIL bad argument '%s' (usage: top of src/hello4/main.c)", a);
			bad = 1;
		}
	}
	return bad ? -1 : 0;
}

static void log_gl_info(void)
{
	const GLubyte *r = glGetString(GL_RENDERER), *v = glGetString(GL_VERSION);
	GLint depth = -1, stencil = -1, lights = -1, tex = -1, stack = -1;

	glGetIntegerv(GL_DEPTH_BITS, &depth);
	glGetIntegerv(GL_STENCIL_BITS, &stencil);
	glGetIntegerv(GL_MAX_LIGHTS, &lights);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex);
	glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, &stack);
	log_msg("GL_RENDERER: %s", r ? (const char *)r : "(null)");
	log_msg("GL_VERSION: %s", v ? (const char *)v : "(null)");
	log_msg("GL depth bits=%d stencil bits=%d max lights=%d max texture=%d modelview stack=%d",
	        depth, stencil, lights, tex, stack);
}

int main(int argc, char *argv[])
{
	char path[256], plan[512];
	struct gfx g;
	struct opts o;
	struct seg seg[MAX_SEGS];
	unsigned run_s = 0, i, n;
	size_t len = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [scene_seconds] [option=value ...]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];

	snprintf(path, sizeof(path), "%s/hello4.log", sd_path);
	if (log_open(path) != 0)
		fprintf(stderr, "hello4: cannot open %s, logging to stdout only\n", path);
	log_msg("hello4 start, pid=%d", getpid());
	if (parse_args(&o, argc, argv) != 0) {
		log_close();
		return 2;
	}
	n = build_segs(&o, seg);
	plan[0] = '\0';
	for (i = 0; i < n && len + 1 < sizeof(plan); i++) {
		if (seg[i].scene == SCENE_STRESS)
			len += snprintf(plan + len, sizeof(plan) - len, " %u tori %u ms,",
			                o.tori[seg[i].step], seg[i].ms);
		else
			len += snprintf(plan + len, sizeof(plan) - len, " %s %u ms,",
			                scene_name[seg[i].scene], seg[i].ms);
		run_s += (seg[i].ms + 999) / 1000;
	}
	for (; i < n; i++)
		run_s += (seg[i].ms + 999) / 1000;
	log_msg("plan: probes %s,%s trace=%d, %u s", o.probes ? "on" : "off", plan, o.trace, run_s);
	quit_init(sd_path, run_s + WATCHDOG_SLACK);

	if (gfx_init(&g, CID) == 0) {
		log_gl_info();
		if (o.probes)
			nprobes = probes_run(g.width, g.height, probe_state);
		setup_lights_fog();
		if (make_assets() == 0)
			run(&g, &o);
	} else {
		log_msg("gfx_init failed, see the last FAIL line above");
	}
	gfx_shutdown(&g);
	free_assets();
	log_msg("hello4 exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

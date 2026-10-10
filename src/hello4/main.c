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
 *   4. stress  1, 2, 4 ... 64 lit tori (1024 triangles each). Logs fps
 *              and triangles/s per step.
 * Top left: frames in the previous second (green) and, in the stress
 * scene, the torus count (yellow). Bottom: scene progress bar.
 *
 * Stops early, with the normal teardown, on a button press, SIGTERM/
 * SIGINT/SIGHUP, SD card removal or the watchdog (src/common/quit.c).
 *
 * Output on the card: hello4.log (GL limits, probe reads + verdicts,
 * per-second fps, triangle-rate table) and layer manager screenshots of
 * scenes 1-3.
 *
 * usage: hello4 <sd_path> [scene_seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

enum { SCENE_CUBE, SCENE_LIGHT, SCENE_WORLD, SCENE_STRESS, NSCENES };
static const char *const scene_name[NSCENES] = { "cube", "light", "world", "stress" };

static const unsigned stress_count[] = { 1, 2, 4, 8, 16, 32, 64 };
#define NSTRESS (sizeof(stress_count) / sizeof(stress_count[0]))

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

	glFogf(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 10.0f);
	glFogf(GL_FOG_END, 32.0f);
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

struct stress_stat {
	unsigned frames, first_ms, last_ms;
};

static void log_stress_table(const struct stress_stat *st)
{
	unsigned i, tris = torus.nidx / 3;

	log_msg("triangle-rate results (lit, smooth, untextured tori, %u tris each):", tris);
	for (i = 0; i < NSTRESS; i++) {
		unsigned span = st[i].last_ms - st[i].first_ms;

		/* the first frame of a step is the warm-up: its end starts the clock */
		if (st[i].frames < 2 || span == 0) {
			log_msg("  %2u tori: too few frames (%u)", stress_count[i], st[i].frames);
			continue;
		}
		log_msg("  %2u tori = %5u tris: %3u frames in %4u ms = %3u.%u fps = %5u ktris/s",
		        stress_count[i], stress_count[i] * tris, st[i].frames - 1, span,
		        (st[i].frames - 1) * 1000 / span, (st[i].frames - 1) * 10000 / span % 10,
		        (unsigned)((unsigned long long)(st[i].frames - 1) * stress_count[i] * tris / span));
	}
}

static void run(struct gfx *g, unsigned scene_s)
{
	unsigned scene_ms = scene_s * 1000, run_ms = NSCENES * scene_ms;
	unsigned start = log_ms(), last = start, next_report = 1000;
	unsigned frames = 0, sec_frames = 0, fps_shown = 0, worst = 0, swap_fail = 0;
	unsigned scene_frames = 0, scene_start = 0, cur_scene = 0, shot_done = 0;
	struct stress_stat stress[NSTRESS];
	int w = g->width, h = g->height;

	memset(stress, 0, sizeof(stress));
	glViewport(0, 0, w, h);
	glDisable(GL_DITHER);
	log_msg("run: %u scenes x %u s, glError=0x%04x", NSCENES, scene_s, glGetError());
	log_msg("scene 1/%u: %s", NSCENES, scene_name[0]);

	for (;;) {
		unsigned now = log_ms(), ms = now - start, scene, sms, count = 0, si = 0;
		float t;

		if (ms >= run_ms)
			quit_request(QUIT_TIMER);
		if (quit_poll() != QUIT_NONE)
			break;
		scene = ms / scene_ms;
		sms = ms % scene_ms;
		t = sms / 1000.0f;
		if (scene != cur_scene) {
			log_msg("scene %u done: %u frames in %u ms (avg %u fps)", cur_scene + 1,
			        scene_frames, ms - scene_start,
			        scene_frames * 1000 / (ms - scene_start ? ms - scene_start : 1));
			log_msg("scene %u/%u: %s", scene + 1, NSCENES, scene_name[scene]);
			cur_scene = scene;
			scene_frames = 0;
			scene_start = ms;
			shot_done = 0;
		}

		switch (scene) {
		case SCENE_CUBE:
			begin_3d(w, h, navy);
			draw_cube_scene(t);
			break;
		case SCENE_LIGHT:
			begin_3d(w, h, black);
			draw_light_scene(t, sms >= scene_ms / 2);
			break;
		case SCENE_WORLD:
			begin_3d(w, h, sky);
			draw_world_scene(t);
			break;
		default:
			si = sms * NSTRESS / scene_ms;
			count = stress_count[si];
			begin_3d(w, h, grey);
			draw_stress_scene(t, count);
			break;
		}
		draw_hud(w, h, fps_shown, count, (float)sms / scene_ms);

		/* screenshot mid-scene; not in the stress scene, it would skew timing */
		if (!shot_done && scene != SCENE_STRESS && sms >= scene_ms / 2) {
			screenshot(scene_name[scene]);
			shot_done = 1;
		}
		if (gfx_swap(g) != 0 && swap_fail++ < 3)
			log_msg("eglSwapBuffers failed: %s", egl_err_name(eglGetError()));

		frames++;
		sec_frames++;
		scene_frames++;
		now = log_ms();
		if (scene == SCENE_STRESS) {
			if (stress[si].frames++ == 0)
				stress[si].first_ms = now;
			stress[si].last_ms = now;
		}
		if (now - last > worst)
			worst = now - last;
		last = now;
		if (ms >= next_report) {
			log_msg("t=%2u s  %-6s fps=%u  worst frame=%u ms  tori=%u  glError=0x%04x",
			        ms / 1000, scene_name[scene], sec_frames, worst, count, glGetError());
			fps_shown = sec_frames;
			sec_frames = 0;
			worst = 0;
			next_report += 1000;
		}
	}
	log_msg("scene %u done: %u frames in %u ms", cur_scene + 1, scene_frames,
	        log_ms() - start - scene_start);
	log_stress_table(stress);
	run_ms = log_ms() - start;   /* actual, shorter after an early stop */
	if (run_ms == 0)
		run_ms = 1;
	log_msg("run done: %u frames in %u ms (avg %u.%u fps), swap failures=%u",
	        frames, run_ms, frames * 1000 / run_ms, frames * 10000 / run_ms % 10, swap_fail);
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
	char path[256];
	struct gfx g;
	unsigned scene_s = SCENE_SECONDS;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [scene_seconds]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	if (argc > 2 && atoi(argv[2]) > 0)
		scene_s = (unsigned)atoi(argv[2]);

	snprintf(path, sizeof(path), "%s/hello4.log", sd_path);
	if (log_open(path) != 0)
		fprintf(stderr, "hello4: cannot open %s, logging to stdout only\n", path);
	log_msg("hello4 start, pid=%d, %u scenes x %u s", getpid(), NSCENES, scene_s);
	quit_init(sd_path, NSCENES * scene_s + WATCHDOG_SLACK);

	if (gfx_init(&g, CID) == 0) {
		log_gl_info();
		nprobes = probes_run(g.width, g.height, probe_state);
		setup_lights_fog();
		if (make_assets() == 0)
			run(&g, scene_s);
	} else {
		log_msg("gfx_init failed, see the last FAIL line above");
	}
	gfx_shutdown(&g);
	free_assets();
	log_msg("hello4 exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

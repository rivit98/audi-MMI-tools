/*
 * hello3 - GPU canvas for the MMI 3G+ (follow-up to hello2).
 *
 * The CPU builds two small textures once at start-up. After that it only
 * sends a few vertices and a texture matrix per frame. Every on-screen
 * pixel is produced by the GPU: texture sampling, filtering, blending.
 * There is no per-frame pixel upload over the PCI bus.
 *
 * Three scenes, each <scene_seconds> long (default 10 s):
 *   1. pixels  random-colour "canvas": a 256x256 noise texture drawn with
 *              GL_NEAREST so each texel is a solid block on screen. Every
 *              100 ms the texture matrix jumps to a random offset, so the
 *              whole screen changes colour. Block size 32 -> 16 -> 8 -> 4 px.
 *   2. plasma  three layers of a smooth tileable texture, each scrolled and
 *              rotated by its own texture matrix and blended together:
 *              flowing colours made by the GPU's filtering and blending.
 *   3. fill    fill-rate benchmark: 1, 2, 4, ... 32 full-screen blended
 *              layers per frame. Logs fps and megapixels/s per step.
 * A seven-segment FPS counter (top left) and a progress bar (bottom)
 * are drawn as untextured quads over every frame.
 *
 * Stops early, with the normal teardown, on a button press, SIGTERM/
 * SIGINT/SIGHUP, SD card removal or the watchdog (src/common/quit.c).
 *
 * Output on the card: hello3.log (per-second fps, per-scene summary,
 * fill-rate table) and layer manager screenshots of scenes 1 and 2.
 *
 * usage: hello3 <sd_path> [scene_seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <GLES/gl.h>

#include "gfx.h"
#include "hud.h"
#include "log.h"
#include "quit.h"

/* openqnx ships no <math.h> (libm is not in the source leak); the device's
 * libm.so.2 exports sinf, and the sysroot has a stub for it. */
float sinf(float x);

#define CID             40     /* displayable id qnx-audi uses */
#define SCENE_SECONDS   10
#define WATCHDOG_SLACK  40     /* stop (then _exit) if GL or the LM hangs */

#define NOISE_SIZE      256    /* texels per side, power of two (GLES 1.0) */
#define PLASMA_SIZE     128
#define PIXELS_STEP_MS  100    /* new random canvas every 100 ms */
#define TWO_PI          6.2831853f

enum { SCENE_PIXELS, SCENE_PLASMA, SCENE_FILL, NSCENES };
static const char *const scene_name[NSCENES] = { "pixels", "plasma", "fill" };

static const unsigned block_px[] = { 32, 16, 8, 4 };
static const unsigned fill_layers[] = { 1, 2, 4, 8, 16, 32 };
#define NBLOCKS (sizeof(block_px) / sizeof(block_px[0]))
#define NFILL   (sizeof(fill_layers) / sizeof(fill_layers[0]))

static const char *sd_path;
static const char *out_dir;    /* <sd_path>/var: everything written */
static GLuint noise_tex, plasma_tex;
static unsigned rng_state;

static unsigned rnd(void)
{
	/* xorshift32 */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* ---- textures (built once by the CPU) ---- */

static GLuint upload_texture(const GLubyte *rgba, int size, GLfloat filter)
{
	GLuint t = 0;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	/* glTexParameterf/glTexEnvf: the GLES 1.0 entry points */
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	return t;
}

/* Fully saturated random colours, so the blocks are clearly distinct. */
static void fill_noise(GLubyte *p, int size)
{
	int i;

	for (i = 0; i < size * size; i++, p += 4) {
		unsigned r = rnd();

		p[0] = r;
		p[1] = r >> 8;
		p[2] = r >> 16;
		p[3] = 255;
		p[(r >> 24) % 3] |= 0xc0;      /* at least one bright channel */
	}
}

/* Tileable plasma: integer frequencies only, so it repeats seamlessly with
 * GL_REPEAT. The value is mapped onto a rainbow palette. */
static void fill_plasma(GLubyte *p, int size)
{
	int x, y;

	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++, p += 4) {
			float fx = TWO_PI * x / size, fy = TWO_PI * y / size;
			float v = (sinf(2 * fx) + sinf(3 * fy) + sinf(fx + 2 * fy) +
			           sinf(3 * fx - fy)) * 0.25f * 3.1415927f;

			p[0] = (GLubyte)(127.5f + 127.4f * sinf(v));
			p[1] = (GLubyte)(127.5f + 127.4f * sinf(v + TWO_PI / 3));
			p[2] = (GLubyte)(127.5f + 127.4f * sinf(v + 2 * TWO_PI / 3));
			p[3] = 255;
		}
	}
}

static int make_textures(void)
{
	GLubyte *buf = malloc(NOISE_SIZE * NOISE_SIZE * 4);
	unsigned t0 = log_ms();

	if (buf == NULL) {
		log_msg("FAIL malloc texture buffer");
		return -1;
	}
	fill_noise(buf, NOISE_SIZE);
	noise_tex = upload_texture(buf, NOISE_SIZE, GL_NEAREST);
	fill_plasma(buf, PLASMA_SIZE);
	plasma_tex = upload_texture(buf, PLASMA_SIZE, GL_LINEAR);
	free(buf);
	log_msg("textures: noise %dx%d (id %u), plasma %dx%d (id %u) in %u ms, glError=0x%04x",
	        NOISE_SIZE, NOISE_SIZE, noise_tex, PLASMA_SIZE, PLASMA_SIZE, plasma_tex,
	        log_ms() - t0, glGetError());
	return noise_tex && plasma_tex ? 0 : -1;
}

/* ---- drawing ---- */

static void begin_textured(GLuint tex)
{
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex);
	glDisableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void begin_flat(void)
{
	glDisable(GL_TEXTURE_2D);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
}

/* Texture matrix: texcoord -> translate(tx,ty) . rotate(deg) . scale(s) */
static void tex_matrix(float tx, float ty, float deg, float s)
{
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glTranslatef(tx, ty, 0.0f);
	glRotatef(deg, 0.0f, 0.0f, 1.0f);
	glScalef(s, s, 1.0f);
	glMatrixMode(GL_MODELVIEW);
}

/* One full-screen quad; the texture spans (0,0)-(tu,tv) across it. */
static void screen_quad(int w, int h, float tu, float tv)
{
	const GLfloat v[8] = { 0, 0,  (GLfloat)w, 0,  0, (GLfloat)h,  (GLfloat)w, (GLfloat)h };
	const GLfloat t[8] = { 0, 0,  tu, 0,  0, tv,  tu, tv };

	glVertexPointer(2, GL_FLOAT, 0, v);
	glTexCoordPointer(2, GL_FLOAT, 0, t);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_pixels(int w, int h, unsigned ms, unsigned scene_ms)
{
	static unsigned step = ~0u;
	static float ox, oy;
	unsigned block = block_px[ms * NBLOCKS / scene_ms];

	if (ms / PIXELS_STEP_MS != step) {
		step = ms / PIXELS_STEP_MS;
		/* whole-texel offsets keep GL_NEAREST blocks aligned to the screen */
		ox = (float)(rnd() % NOISE_SIZE) / NOISE_SIZE;
		oy = (float)(rnd() % NOISE_SIZE) / NOISE_SIZE;
	}
	glDisable(GL_BLEND);
	begin_textured(noise_tex);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	tex_matrix(ox, oy, 0.0f, 1.0f);
	screen_quad(w, h, (float)w / block / NOISE_SIZE, (float)h / block / NOISE_SIZE);
}

static void draw_plasma(int w, int h, unsigned ms)
{
	float t = ms / 1000.0f;

	begin_textured(plasma_tex);
	glDisable(GL_BLEND);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	tex_matrix(t * 0.05f, t * 0.03f, t * 6.0f, 1.0f);
	screen_quad(w, h, 2.0f, 1.2f);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
	tex_matrix(-t * 0.04f, t * 0.07f, -t * 9.0f, 1.5f);
	screen_quad(w, h, 2.0f, 1.2f);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE);          /* additive highlight */
	glColor4f(0.6f, 0.4f, 1.0f, 0.35f);
	tex_matrix(t * 0.02f, -t * 0.05f, t * 3.0f, 0.7f);
	screen_quad(w, h, 2.0f, 1.2f);
	glDisable(GL_BLEND);
}

static void draw_fill(int w, int h, unsigned layers, unsigned ms)
{
	float t = ms / 1000.0f;
	unsigned i;

	begin_textured(plasma_tex);
	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (i = 0; i < layers; i++) {
		if (i == 1)
			glEnable(GL_BLEND);   /* layer 0 is opaque, the rest blend */
		glColor4f(1.0f, 1.0f, 1.0f, i ? 0.3f : 1.0f);
		tex_matrix(i * 0.13f + t * 0.05f, i * 0.29f, i * 11.0f + t * 5.0f, 1.0f);
		screen_quad(w, h, 2.0f, 1.2f);
	}
	glDisable(GL_BLEND);
}

static void draw_hud(int w, int h, unsigned fps, unsigned layers, float progress)
{
	static const GLubyte backing[4] = { 0, 0, 0, 170 };
	static const GLubyte green[4] = { 80, 255, 80, 255 };
	static const GLubyte yellow[4] = { 255, 200, 0, 255 };

	begin_flat();
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	hud_rect(8.0f, h - 56.0f, layers ? 210.0f : 110.0f, 48.0f, backing);
	glDisable(GL_BLEND);
	hud_number(18.0f, h - 48.0f, fps, 3, 18.0f, 32.0f, green);
	if (layers)
		hud_number(118.0f, h - 48.0f, layers, 3, 18.0f, 32.0f, yellow);
	hud_rect(0.0f, 0.0f, w * progress, 8.0f, yellow);
}

/* ---- main loop ---- */

static void screenshot(const char *scene)
{
	char cmd[320];
	int rc;

	snprintf(cmd, sizeof(cmd), "s-shot 0 %s/hello3_%s.png", out_dir, scene);
	rc = lm_text_command(cmd);
	log_msg("layermanager '%s': %s", cmd, rc ? strerror(rc) : "ok");
}

struct fill_stat {
	unsigned frames, first_ms, last_ms;
};

static void log_fill_table(const struct fill_stat *st, int w, int h)
{
	unsigned i;

	log_msg("fill-rate results (%dx%d, textured + blended full-screen layers):", w, h);
	for (i = 0; i < NFILL; i++) {
		unsigned span = st[i].last_ms - st[i].first_ms;
		unsigned long long px;

		/* the first frame of a step is the warm-up: its end starts the clock */
		if (st[i].frames < 2 || span == 0) {
			log_msg("  %2u layers: too few frames (%u)", fill_layers[i], st[i].frames);
			continue;
		}
		px = (unsigned long long)(st[i].frames - 1) * fill_layers[i] * w * h;
		log_msg("  %2u layers: %3u frames in %4u ms = %3u.%u fps = %4u Mpixel/s",
		        fill_layers[i], st[i].frames - 1, span,
		        (st[i].frames - 1) * 1000 / span, (st[i].frames - 1) * 10000 / span % 10,
		        (unsigned)(px / span / 1000));
	}
}

static void run(struct gfx *g, unsigned scene_s)
{
	unsigned scene_ms = scene_s * 1000, run_ms = NSCENES * scene_ms;
	unsigned start = log_ms(), last = start, next_report = 1000;
	unsigned frames = 0, sec_frames = 0, fps_shown = 0, worst = 0, swap_fail = 0;
	unsigned scene_frames = 0, scene_start = 0, cur_scene = 0, shot_done = 0;
	/* Milliseconds spent on screenshots. Kept out of the scene clock and of
	 * the fps / worst-frame numbers: capturing is not drawing. */
	unsigned paused = 0;
	struct fill_stat fill[NFILL];
	int w = g->width, h = g->height;

	memset(fill, 0, sizeof(fill));
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0.0f, (GLfloat)w, 0.0f, (GLfloat)h, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);
	glEnableClientState(GL_VERTEX_ARRAY);
	log_msg("run: %u scenes x %u s, glError=0x%04x", NSCENES, scene_s, glGetError());
	log_msg("scene 1/%u: %s", NSCENES, scene_name[0]);

	for (;;) {
		unsigned now = log_ms(), ms = now - start - paused, scene, sms, layers = 0, fi = 0;

		if (ms >= run_ms)
			quit_request(QUIT_TIMER);
		if (quit_poll() != QUIT_NONE)
			break;
		scene = ms / scene_ms;
		sms = ms % scene_ms;
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
		case SCENE_PIXELS:
			draw_pixels(w, h, sms, scene_ms);
			break;
		case SCENE_PLASMA:
			draw_plasma(w, h, sms);
			break;
		default:
			fi = sms * NFILL / scene_ms;
			layers = fill_layers[fi];
			draw_fill(w, h, layers, sms);
			break;
		}
		draw_hud(w, h, fps_shown, layers, (float)sms / scene_ms);

		/* screenshot mid-scene; not in the fill scene, it would skew timing */
		if (!shot_done && scene != SCENE_FILL && sms >= scene_ms / 2) {
			unsigned t0 = log_ms();

			screenshot(scene_name[scene]);
			paused += log_ms() - t0;
			last = log_ms();        /* don't bill the capture to this frame */
			shot_done = 1;
		}
		if (gfx_swap(g) != 0 && swap_fail++ < 3)
			log_msg("eglSwapBuffers failed: %s", egl_err_name(eglGetError()));

		frames++;
		sec_frames++;
		scene_frames++;
		now = log_ms();
		if (scene == SCENE_FILL) {
			if (fill[fi].frames++ == 0)
				fill[fi].first_ms = now;
			fill[fi].last_ms = now;
		}
		if (now - last > worst)
			worst = now - last;
		last = now;
		if (ms >= next_report) {
			log_msg("t=%2u s  %-6s fps=%u  worst frame=%u ms  layers=%u  glError=0x%04x",
			        ms / 1000, scene_name[scene], sec_frames, worst, layers, glGetError());
			fps_shown = sec_frames;
			sec_frames = 0;
			worst = 0;
			next_report += 1000;
		}
	}
	log_msg("scene %u done: %u frames in %u ms", cur_scene + 1, scene_frames,
	        log_ms() - start - paused - scene_start);
	log_fill_table(fill, w, h);
	run_ms = log_ms() - start - paused;   /* actual, shorter after an early stop */
	if (run_ms == 0)
		run_ms = 1;
	log_msg("run done: %u frames in %u ms (avg %u.%u fps), swap failures=%u",
	        frames, run_ms, frames * 1000 / run_ms, frames * 10000 / run_ms % 10, swap_fail);
	if (paused != 0)
		log_msg("  (+%u ms capturing and writing files, not counted above)", paused);
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
	out_dir = var_dir(sd_path);
	if (argc > 2 && atoi(argv[2]) > 0)
		scene_s = (unsigned)atoi(argv[2]);

	snprintf(path, sizeof(path), "%s/hello3.log", out_dir);
	if (log_open(path) != 0)
		fprintf(stderr, "hello3: cannot open %s, logging to stdout only\n", path);
	rng_state = (0x9e3779b9u ^ ((unsigned)getpid() << 8) ^ log_ms()) | 1;
	log_msg("hello3 start, pid=%d, %u scenes x %u s, seed=0x%08x",
	        getpid(), NSCENES, scene_s, rng_state);
	quit_init(sd_path, NSCENES * scene_s + WATCHDOG_SLACK);

	if (gfx_init(&g, CID) == 0) {
		const GLubyte *r = glGetString(GL_RENDERER), *v = glGetString(GL_VERSION);

		log_msg("GL_RENDERER: %s", r ? (const char *)r : "(null)");
		log_msg("GL_VERSION: %s", v ? (const char *)v : "(null)");
		if (make_textures() == 0)
			run(&g, scene_s);
	} else {
		log_msg("gfx_init failed, see the last FAIL line above");
	}
	gfx_shutdown(&g);
	log_msg("hello3 exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

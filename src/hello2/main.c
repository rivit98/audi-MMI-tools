/*
 * hello2 - OpenGL ES verification app for the MMI 3G+.
 *
 *  1. Brings up OpenGL ES (ES-CM 1.0) on the screen (src/common/gfx.c) and
 *     logs every step plus the EGL/GL diagnostics to <sd>/hello2.log.
 *  2. Animates for RUN_SECONDS: hue-cycling background, rotating gradient
 *     square, bouncing block, progress bar along the bottom.
 *  3. At SHOT_AT_MS captures what it rendered three ways:
 *       hello2_glread.bmp   glReadPixels of our own frame (GL works)
 *       hello2_dshot.png    layer manager snapshot of our displayable's VFB
 *       hello2_sshot.png    layer manager snapshot of the whole display
 *                           (proves our layer is what's actually visible)
 *  4. Hands the screen back like qnx-audi does and exits. Stops early, with
 *     the same clean teardown, on a button press, SIGTERM/SIGINT/SIGHUP,
 *     SD card removal or the watchdog (src/common/quit.c).
 *
 * usage: hello2 <sd_path> [seconds]
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <GLES/gl.h>

#include "img.h"
#include "gfx.h"
#include "log.h"
#include "quit.h"

#define CID           40    /* displayable id qnx-audi uses */
#define RUN_SECONDS   20
#define SHOT_AT_MS    5000
#define WATCHDOG_SLACK 40   /* stop (then _exit) if GL or the LM hangs */

static const char *sd_path;

static void log_gl_string(const char *name, GLenum e)
{
	const GLubyte *s = glGetString(e);

	log_msg("%s: %s", name, s ? (const char *)s : "(null)");
}

static void log_gl_ints(const char *name, GLenum e, int count)
{
	GLint v[2] = { -1, -1 };

	glGetIntegerv(e, v);
	if (count == 2)
		log_msg("%s: %d %d", name, v[0], v[1]);
	else
		log_msg("%s: %d", name, v[0]);
}

static void log_gl_info(void)
{
	log_gl_string("GL_VENDOR", GL_VENDOR);
	log_gl_string("GL_RENDERER", GL_RENDERER);
	log_gl_string("GL_VERSION", GL_VERSION);
	log_gl_string("GL_EXTENSIONS", GL_EXTENSIONS);
	log_gl_ints("GL_MAX_TEXTURE_SIZE", GL_MAX_TEXTURE_SIZE, 1);
	log_gl_ints("GL_MAX_TEXTURE_UNITS", GL_MAX_TEXTURE_UNITS, 1);
	log_gl_ints("GL_MAX_LIGHTS", GL_MAX_LIGHTS, 1);
	log_gl_ints("GL_MAX_CLIP_PLANES", GL_MAX_CLIP_PLANES, 1);
	log_gl_ints("GL_MAX_MODELVIEW_STACK_DEPTH", GL_MAX_MODELVIEW_STACK_DEPTH, 1);
	log_gl_ints("GL_MAX_PROJECTION_STACK_DEPTH", GL_MAX_PROJECTION_STACK_DEPTH, 1);
	log_gl_ints("GL_MAX_VIEWPORT_DIMS", GL_MAX_VIEWPORT_DIMS, 2);
	log_gl_ints("GL_ALIASED_POINT_SIZE_RANGE", GL_ALIASED_POINT_SIZE_RANGE, 2);
	log_gl_ints("GL_ALIASED_LINE_WIDTH_RANGE", GL_ALIASED_LINE_WIDTH_RANGE, 2);
	log_gl_ints("GL_RED_BITS", GL_RED_BITS, 1);
	log_gl_ints("GL_GREEN_BITS", GL_GREEN_BITS, 1);
	log_gl_ints("GL_BLUE_BITS", GL_BLUE_BITS, 1);
	log_gl_ints("GL_ALPHA_BITS", GL_ALPHA_BITS, 1);
	log_gl_ints("GL_DEPTH_BITS", GL_DEPTH_BITS, 1);
	log_gl_ints("GL_STENCIL_BITS", GL_STENCIL_BITS, 1);
	log_gl_ints("GL_SUBPIXEL_BITS", GL_SUBPIXEL_BITS, 1);
	log_gl_ints("GL_NUM_COMPRESSED_TEXTURE_FORMATS", GL_NUM_COMPRESSED_TEXTURE_FORMATS, 1);
	log_msg("glGetError after queries: 0x%04x", glGetError());
}

/* h in [0,1), s/v in [0,1] -> rgb in [0,1] */
static void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b)
{
	float f = h * 6.0f - (int)(h * 6.0f);
	float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));

	switch ((int)(h * 6.0f) % 6) {
	case 0:  *r = v; *g = t; *b = p; break;
	case 1:  *r = q; *g = v; *b = p; break;
	case 2:  *r = p; *g = v; *b = t; break;
	case 3:  *r = p; *g = q; *b = v; break;
	case 4:  *r = t; *g = p; *b = v; break;
	default: *r = v; *g = p; *b = q; break;
	}
}

static void draw_rect(float x, float y, float w, float h, const GLubyte rgba[16])
{
	const GLfloat v[8] = { x, y,  x + w, y,  x, y + h,  x + w, y + h };

	glVertexPointer(2, GL_FLOAT, 0, v);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_frame(int w, int h, unsigned ms, unsigned run_ms)
{
	static const GLubyte gradient[16] = {
		255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   255, 255, 0, 255,
	};
	static const GLubyte white[16] = {
		255, 255, 255, 255, 255, 255, 255, 255,
		255, 255, 255, 255, 255, 255, 255, 255,
	};
	static const GLubyte yellow[16] = {
		255, 200, 0, 255, 255, 200, 0, 255, 255, 200, 0, 255, 255, 200, 0, 255,
	};
	float t = ms / 1000.0f, r, g, b, x;
	unsigned period = 3000, phase = ms % (2 * period);

	/* background: full hue cycle every 10 s */
	hsv_to_rgb(t / 10.0f - (int)(t / 10.0f), 0.6f, 0.45f, &r, &g, &b);
	glClearColor(r, g, b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* rotating gradient square in the centre, 90 deg/s */
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(w / 2.0f, h / 2.0f, 0.0f);
	glRotatef(t * 90.0f, 0.0f, 0.0f, 1.0f);
	draw_rect(-100.0f, -100.0f, 200.0f, 200.0f, gradient);

	/* white block bouncing left-right along the top, 3 s per pass */
	glLoadIdentity();
	x = (phase < period ? phase : 2 * period - phase) / (float)period * (w - 40);
	draw_rect(x, h - 70.0f, 40.0f, 40.0f, white);

	/* progress bar: how much of the run is left */
	draw_rect(0.0f, 0.0f, w * (float)ms / run_ms, 10.0f, yellow);
}

static void take_screenshots(int w, int h)
{
	char path[256], cmd[320];
	unsigned char *pixels = malloc((size_t)w * h * 4);
	int rc;

	if (pixels != NULL) {
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		rc = glGetError();
		snprintf(path, sizeof(path), "%s/hello2_glread.bmp", sd_path);
		log_msg("glReadPixels %dx%d: glError=0x%04x, centre pixel rgba=%u,%u,%u,%u",
		        w, h, rc,
		        pixels[((h / 2) * w + w / 2) * 4 + 0], pixels[((h / 2) * w + w / 2) * 4 + 1],
		        pixels[((h / 2) * w + w / 2) * 4 + 2], pixels[((h / 2) * w + w / 2) * 4 + 3]);
		log_msg("write %s: %s", path,
		        img_write_bmp_rgba(path, pixels, w, h) == 0 ? "ok" : strerror(errno));
		free(pixels);
	}

	snprintf(cmd, sizeof(cmd), "d-shot %d %s/hello2_dshot.png", CID, sd_path);
	rc = lm_text_command(cmd);
	log_msg("layermanager '%s': %s", cmd, rc ? strerror(rc) : "ok");
	snprintf(cmd, sizeof(cmd), "s-shot 0 %s/hello2_sshot.png", sd_path);
	rc = lm_text_command(cmd);
	log_msg("layermanager '%s': %s", cmd, rc ? strerror(rc) : "ok");
}

static void animate(struct gfx *g, unsigned seconds)
{
	unsigned run_ms = seconds * 1000, start = log_ms(), last = start;
	unsigned frames = 0, sec_frames = 0, next_report = 1000;
	unsigned worst = 0, swap_fail = 0, shot_done = 0;
	/* Milliseconds spent capturing and writing files. The readback and the
	 * card write are not drawing, so they are kept out of both the clock
	 * that drives the animation and the fps / worst-frame numbers. */
	unsigned paused = 0;

	glViewport(0, 0, g->width, g->height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0.0f, (GLfloat)g->width, 0.0f, (GLfloat)g->height, -1.0f, 1.0f);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glDisable(GL_DEPTH_TEST);
	log_msg("animation: %u s, glError=0x%04x", seconds, glGetError());

	for (;;) {
		unsigned now = log_ms(), ms = now - start - paused;

		if (ms >= run_ms)
			quit_request(QUIT_TIMER);
		if (quit_poll() != QUIT_NONE)
			break;
		draw_frame(g->width, g->height, ms, run_ms);
		if (!shot_done && ms >= SHOT_AT_MS) {
			unsigned t0 = log_ms();

			take_screenshots(g->width, g->height);
			paused += log_ms() - t0;
			last = log_ms();        /* don't bill the capture to this frame */
			shot_done = 1;
		}
		if (gfx_swap(g) != 0 && swap_fail++ < 3)
			log_msg("eglSwapBuffers failed: %s", egl_err_name(eglGetError()));

		frames++;
		sec_frames++;
		now = log_ms();
		if (now - last > worst)
			worst = now - last;
		last = now;
		if (ms >= next_report) {
			log_msg("t=%2u s  fps=%u  worst frame=%u ms  glError=0x%04x",
			        ms / 1000, sec_frames, worst, glGetError());
			sec_frames = 0;
			worst = 0;
			next_report += 1000;
		}
	}
	run_ms = log_ms() - start - paused;   /* actual, shorter after an early stop */
	if (run_ms == 0)
		run_ms = 1;
	log_msg("animation done: %u frames in %u ms (avg %u.%u fps), swap failures=%u",
	        frames, run_ms, frames * 1000 / run_ms, frames * 10000 / run_ms % 10, swap_fail);
	if (paused != 0)
		log_msg("  (+%u ms capturing and writing files, not counted above)", paused);
}

int main(int argc, char *argv[])
{
	char path[256];
	struct utsname u;
	struct gfx g;
	unsigned seconds = RUN_SECONDS;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [seconds]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	if (argc > 2 && atoi(argv[2]) > 0)
		seconds = (unsigned)atoi(argv[2]);

	snprintf(path, sizeof(path), "%s/hello2.log", sd_path);
	if (log_open(path) != 0)
		fprintf(stderr, "hello2: cannot open %s, logging to stdout only\n", path);
	log_msg("hello2 start, pid=%d, run=%u s", getpid(), seconds);
	if (uname(&u) == 0)
		log_msg("uname: %s %s %s %s %s", u.sysname, u.nodename, u.release,
		        u.version, u.machine);
	quit_init(sd_path, seconds + WATCHDOG_SLACK);

	if (gfx_init(&g, CID) == 0) {
		log_gl_info();
		animate(&g, seconds);
	} else {
		log_msg("gfx_init failed, see the last FAIL line above");
	}
	gfx_shutdown(&g);
	log_msg("hello2 exit (stop reason: %s)", quit_name(quit_poll()));
	log_close();
	return 0;
}

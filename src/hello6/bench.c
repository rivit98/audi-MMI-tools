/*
 * bench.c - how fast can pixels get onto this screen? The numbers that
 * choose between the raw-pixel mirroring options (docs/phone-mirroring.md
 * section 2.4 and test T3):
 *
 *   CPU   memcpy, RGB565 -> BGRA8888 conversion, LZ4 compress/decompress on
 *         a UI-like, a photo-like and a random 800x480 RGB565 frame
 *   VFB   32-bit CPU stores into the layer manager frame buffer, memcpy into
 *         it and out of it (lmprobe's fill hinted at ~10 MB/s)
 *   GL    glTexSubImage2D RGB565 / RGBA / BGRA and glCompressedTexSubImage2D
 *         DXT1 upload speed, then the visible pipeline (upload + textured
 *         quad + swap) in fps for full frames, a moving 120-line band, DXT1
 *
 * Every result is a "RESULT <name> ..." log line plus a summary line.
 * Times are CLOCK_MONOTONIC milliseconds (log_ms), so every measurement
 * runs long enough (>= ~200 ms) for that to be accurate.
 *
 * The whole unit can freeze somewhere between vfb_memcpy_out and the
 * end of the first RGB565 upload (the first glTexSubImage2D any of our apps
 * makes). So every GL call that moves data now has a "gl:" step line
 * (fsync'ed) before it, and each format's first upload grows 64x64 ->
 * 256x256 -> 800x120 -> 800x480 with a glFinish after each: the last line
 * on the card names the call, size and format that hung.
 */
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES
#include <GLES/gl.h>
#include <GLES/glext.h>

#include "gfx.h"
#include "hello6.h"
#include "log.h"
#include "lz4mini.h"
#include "quit.h"

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

#define W        SCREEN_W
#define H        SCREEN_H
#define PIX      (W * H)
#define TEX_W    1024
#define TEX_H    512
#define CID      40
#define BAND_H   120
#define PIPE_MS  4000

static u32 rng = 1;

static unsigned rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 16;
}

/* MB/s; elapsed ms is never 0 here (a 0 would be a fatal SIGFPE on this
 * platform, docs/qnx-sh-fpu-div0.md). */
static f64 mbps(f64 bytes, unsigned ms)
{
	return bytes / 1048576.0 * 1000.0 / (ms ? ms : 1);
}

static u16 rgb565(unsigned r, unsigned g, unsigned b)
{
	return (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

/* ------------------------------------------------------------------ */
/* test frames                                                         */

/* UI-like: flat background, flat panels, rows of glyph-like 1-bit cells. */
static void frame_ui(u16 *f)
{
	int i, x, y, k;

	for (i = 0; i < PIX; i++)
		f[i] = rgb565(16, 24, 40);
	for (k = 0; k < 12; k++) {
		int x0 = (int)(rnd() % (W - 200)), y0 = (int)(rnd() % (H - 100));
		u16 c = rgb565(rnd() & 255, rnd() & 255, rnd() & 255);

		for (y = y0; y < y0 + 60 + (int)(rnd() % 40); y++)
			for (x = x0; x < x0 + 100 + (int)(rnd() % 100); x++)
				f[y * W + x] = c;
	}
	for (y = 300; y < 460; y += 20) {          /* "text": 8x16 cells */
		for (x = 16; x < W - 16; x += 8) {
			unsigned bits = rnd() | rnd() << 15;

			if (rnd() % 6 == 0)
				continue;                   /* a space */
			for (k = 0; k < 8 * 16; k++)
				if ((bits >> (k % 30)) & (k & 1))
					f[(y + k / 8) * W + x + k % 8] = 0xffff;
		}
	}
}

/* Photo-like: smooth gradients plus sensor-style noise in the low bits. */
static void frame_photo(u16 *f)
{
	int x, y;

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			f[y * W + x] = rgb565((x * 255 / W + (rnd() & 7)) & 255,
			                      (y * 255 / H + (rnd() & 7)) & 255,
			                      ((x + y) * 255 / (W + H) + (rnd() & 7)) & 255);
}

static void frame_random(u16 *f)
{
	int i;

	for (i = 0; i < PIX; i++)
		f[i] = (u16)rnd();
}

/* Visible pattern for the GL pipeline: colour bars with a moving checker
 * strip, variant v shifts everything so consecutive frames differ. */
static void frame_bars(u16 *f, int v)
{
	static const u16 bars[8] = {
		0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x0000,
	};
	int x, y;

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			int xs = (x + v * 37) % W;

			f[y * W + x] = (y > 200 && y < 280)
			    ? ((((x + v * 16) / 16) ^ (y / 16)) & 1 ? 0xffff : 0x0000)
			    : bars[xs * 8 / W];
		}
}

/* DXT1 blocks (4x4 texels, 8 bytes): colour0 from a block gradient,
 * colour1 black, 2-bit indices drawing diagonal stripes; v moves them. */
static void frame_dxt1(u8 *d, int v)
{
	int bx, by;

	for (by = 0; by < H / 4; by++)
		for (bx = 0; bx < W / 4; bx++) {
			u16 c0 = rgb565(bx * 255 / (W / 4), by * 255 / (H / 4), 160);
			u32 idx = 0;
			int t;

			if (c0 == 0)
				c0 = 1;                 /* c0 > c1: 4-colour mode */
			for (t = 0; t < 16; t++)
				idx |= (u32)(((t % 4 + t / 4 + bx + by + v) / 2) & 3) << (2 * t);
			d[0] = (u8)c0;
			d[1] = (u8)(c0 >> 8);
			d[2] = d[3] = 0;
			d[4] = (u8)idx;
			d[5] = (u8)(idx >> 8);
			d[6] = (u8)(idx >> 16);
			d[7] = (u8)(idx >> 24);
			d += 8;
		}
}

/* ------------------------------------------------------------------ */
/* CPU                                                                 */

static void lz4_case(const char *name, const u16 *frame, u8 *cbuf,
                     int ccap, u8 *dbuf)
{
	const int n = PIX * 2;
	unsigned t0, cms, dms;
	int clen, dlen = 0, i, reps = 0;

	t0 = log_ms();
	clen = lz4mini_compress((const u8 *)frame, n, cbuf, ccap);
	cms = log_ms() - t0;
	if (clen < 0) {
		log_msg("FAIL lz4 %s: compress returned %d", name, clen);
		return;
	}
	t0 = log_ms();
	do {
		for (i = 0; i < 4; i++)
			dlen = lz4mini_decompress(cbuf, clen, dbuf, n);
		reps += 4;
	} while (log_ms() - t0 < 300);
	dms = log_ms() - t0;
	if (dlen != n || memcmp(dbuf, frame, (usize)n) != 0) {
		log_msg("FAIL lz4 %s: round trip mismatch (%d of %d bytes)", name, dlen, n);
		return;
	}
	log_msg("RESULT lz4_%s: %d -> %d bytes (%.1f%%), compress %u ms, "
	        "decompress %.1f MB/s (%.1f ms/frame)",
	        name, n, clen, 100.0 * clen / n, cms, mbps((f64)n * reps, dms),
	        (f64)dms / reps);
	summary_add("lz4 %-6s %4.1f%% of size, decode %5.1f ms/frame (%.0f MB/s)",
	            name, 100.0 * clen / n, (f64)dms / reps,
	            mbps((f64)n * reps, dms));
}

void bench_cpu(void)
{
	const int ccap = LZ4MINI_BOUND(PIX * 2);
	u8 *a = malloc(PIX * 4), *b = malloc(PIX * 4);
	u8 *cbuf = malloc((usize)ccap), *dbuf = malloc(PIX * 2);
	u16 *f = malloc(PIX * 2);
	unsigned t0, ms;
	int i, reps;

	log_msg("== CPU benchmarks");
	if (a == NULL || b == NULL || cbuf == NULL || dbuf == NULL || f == NULL) {
		log_msg("FAIL out of memory for the CPU benchmarks");
		goto out;
	}
	memset(a, 0x5a, PIX * 4);

	/* The barrier keeps GCC from dropping copies into a buffer nobody reads:
	 * without it the loop "measures" 860 GB/s. */
	t0 = log_ms();
	for (reps = 0; reps < 1 || log_ms() - t0 < 300; reps++) {
		memcpy(b, a, PIX * 4);
		__asm__ __volatile__("" : : "r"(b) : "memory");
	}
	ms = log_ms() - t0;
	log_msg("RESULT ram_memcpy: %.1f MB/s (%d x %d bytes in %u ms)",
	        mbps((f64)PIX * 4 * reps, ms), reps, PIX * 4, ms);
	summary_add("RAM memcpy        %6.1f MB/s", mbps((f64)PIX * 4 * reps, ms));

	t0 = log_ms();
	for (reps = 0; reps < 1 || log_ms() - t0 < 300; reps++) {
		u32 *p = (u32 *)b;

		for (i = 0; i < PIX; i++)
			p[i] = (u32)i;
		__asm__ __volatile__("" : : "r"(p) : "memory");
	}
	ms = log_ms() - t0;
	log_msg("RESULT ram_store32: %.1f MB/s", mbps((f64)PIX * 4 * reps, ms));

	frame_ui(f);
	t0 = log_ms();
	for (reps = 0; reps < 1 || log_ms() - t0 < 300; reps++) {
		u32 *p = (u32 *)b;

		for (i = 0; i < PIX; i++) {
			unsigned c = f[i];

			p[i] = 0xff000000u | (c & 0xf800) << 8 | (c & 0x07e0) << 5 | (c & 0x001f) << 3;
		}
		__asm__ __volatile__("" : : "r"(p) : "memory");
	}
	ms = log_ms() - t0;
	log_msg("RESULT rgb565_to_bgra: %.1f ms per 800x480 frame", (f64)ms / reps);
	summary_add("RGB565->BGRA      %6.1f ms/frame", (f64)ms / reps);

	lz4_case("ui", f, cbuf, ccap, dbuf);
	frame_photo(f);
	lz4_case("photo", f, cbuf, ccap, dbuf);
	frame_random(f);
	lz4_case("random", f, cbuf, ccap, dbuf);
out:
	free(a);
	free(b);
	free(cbuf);
	free(dbuf);
	free(f);
}

/* ------------------------------------------------------------------ */
/* VFB (CPU access to the layer manager frame buffer)                  */

static void bench_vfb(struct gfx *g, const u8 *ram)
{
	gf_surface_info_t si;
	unsigned t0, ms;
	int x, y, reps;

	memset(&si, 0, sizeof(si));
	gf_surface_get_info(g->surface, &si);
	if (si.vaddr == NULL || si.w < W || si.h < H || si.stride < W * 4) {
		log_msg("FAIL VFB has no usable CPU mapping (vaddr=%p %dx%d stride %u)",
		        (void *)si.vaddr, si.w, si.h, si.stride);
		return;
	}

	t0 = log_ms();
	for (reps = 0; reps < 3 || log_ms() - t0 < 300; reps++)
		for (y = 0; y < H; y++) {
			u32 *row = (u32 *)(si.vaddr + (unsigned)y * si.stride);

			for (x = 0; x < W; x++)
				row[x] = 0xff000000u | (unsigned)(x + reps * 64);
		}
	ms = log_ms() - t0;
	log_msg("RESULT vfb_store32: %.1f MB/s, %.1f ms per frame",
	        mbps((f64)PIX * 4 * reps, ms), (f64)ms / reps);
	summary_add("VFB CPU store32   %6.1f MB/s (%.0f ms/frame)",
	            mbps((f64)PIX * 4 * reps, ms), (f64)ms / reps);

	t0 = log_ms();
	for (reps = 0; reps < 3 || log_ms() - t0 < 300; reps++)
		for (y = 0; y < H; y++)
			memcpy(si.vaddr + (unsigned)y * si.stride, ram + y * W * 4, W * 4);
	ms = log_ms() - t0;
	log_msg("RESULT vfb_memcpy_in: %.1f MB/s, %.1f ms per frame",
	        mbps((f64)PIX * 4 * reps, ms), (f64)ms / reps);
	summary_add("VFB memcpy in     %6.1f MB/s (%.0f ms/frame)",
	            mbps((f64)PIX * 4 * reps, ms), (f64)ms / reps);

	t0 = log_ms();
	for (reps = 0; reps < 2 || log_ms() - t0 < 300; reps++)
		for (y = 0; y < H; y++)
			memcpy((u8 *)ram + y * W * 4, si.vaddr + (unsigned)y * si.stride, W * 4);
	ms = log_ms() - t0;
	log_msg("RESULT vfb_memcpy_out: %.1f MB/s", mbps((f64)PIX * 4 * reps, ms));
	summary_add("VFB memcpy out    %6.1f MB/s", mbps((f64)PIX * 4 * reps, ms));
	log_msg("gl: lm_update_vfb(%d)", g->cid);
	log_msg("gl:   rc=%d", lm_update_vfb(g->cid));
}

/* ------------------------------------------------------------------ */
/* GL                                                                  */

static GLuint new_texture(void)
{
	GLuint t;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

/* glTexSubImage2D of a full 800x480 frame into a 1024x512 texture. */
static void upload_case(const char *name, GLenum fmt, GLenum type, int bpp,
                        const void *f0, const void *f1, const void *zero)
{
	static const int ladder[][2] = { { 64, 64 }, { 256, 256 }, { W, 120 }, { W, H } };
	GLuint t = new_texture();
	GLenum err;
	unsigned t0, ms;
	int reps, k;

	log_msg("gl: upload_%s: glTexImage2D %dx%d", name, TEX_W, TEX_H);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, TEX_W, TEX_H, 0, fmt, type, zero);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		log_msg("RESULT upload_%s: glTexImage2D error 0x%04x (format not supported)",
		        name, err);
		summary_add("upload %-8s not supported (0x%04x)", name, err);
		glDeleteTextures(1, &t);
		return;
	}
	glFinish();
	for (k = 0; k < (int)(sizeof(ladder) / sizeof(ladder[0])); k++) {
		log_msg("gl: upload_%s: glTexSubImage2D %dx%d + glFinish", name,
		        ladder[k][0], ladder[k][1]);
		t0 = log_ms();
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ladder[k][0], ladder[k][1], fmt, type, f0);
		glFinish();
		log_msg("gl:   ok in %u ms, glError 0x%04x", log_ms() - t0, glGetError());
	}
	/* glFinish after EVERY upload: the whole unit freezes in this
	 * loop when up to four 800x480 uploads queue up between
	 * glFinish calls, while the same uploads one at a time with a
	 * glFinish each pass (see docs/gpu-results.md). */
	log_msg("gl: upload_%s: timed loop (glFinish after each)", name);
	t0 = log_ms();
	for (reps = 0; reps < 4 || log_ms() - t0 < 400; reps++) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, fmt, type, reps & 1 ? f1 : f0);
		glFinish();
	}
	ms = log_ms() - t0;
	err = glGetError();
	log_msg("RESULT upload_%s: %.1f MB/s, %.1f ms per 800x480 frame, glError 0x%04x",
	        name, mbps((f64)PIX * bpp * reps, ms), (f64)ms / reps, err);
	summary_add("upload %-8s   %6.1f MB/s (%.1f ms/frame)%s", name,
	            mbps((f64)PIX * bpp * reps, ms), (f64)ms / reps,
	            err ? " GL ERROR" : "");
	glDeleteTextures(1, &t);
}

static void upload_dxt1(const u8 *d0, const u8 *d1, const void *zero)
{
	const int full = TEX_W * TEX_H / 2, part = PIX / 2;
	GLuint t = new_texture();
	GLenum err;
	unsigned t0, ms;
	int reps, sub = 1;

	log_msg("gl: upload_dxt1: glCompressedTexImage2D %dx%d", TEX_W, TEX_H);
	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
	                       TEX_W, TEX_H, 0, full, zero);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		log_msg("RESULT upload_dxt1: glCompressedTexImage2D error 0x%04x", err);
		summary_add("upload dxt1     not supported (0x%04x)", err);
		glDeleteTextures(1, &t);
		return;
	}
	log_msg("gl: upload_dxt1: glCompressedTexSubImage2D %dx%d + glFinish", W, H);
	glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H,
	                          GL_COMPRESSED_RGB_S3TC_DXT1_EXT, part, d0);
	glFinish();
	err = glGetError();
	if (err != GL_NO_ERROR) {
		log_msg("glCompressedTexSubImage2D(DXT1 800x480): error 0x%04x, "
		        "timing whole-texture glCompressedTexImage2D instead", err);
		sub = 0;
	}
	glFinish();
	t0 = log_ms();
	for (reps = 0; reps < 4 || log_ms() - t0 < 400; reps++) {
		if (sub)
			glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H,
			                          GL_COMPRESSED_RGB_S3TC_DXT1_EXT, part,
			                          reps & 1 ? d1 : d0);
		else
			glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
			                       TEX_W, TEX_H, 0, full, zero);
		glFinish();                    /* one upload in flight, see upload_case */
	}
	ms = log_ms() - t0;
	err = glGetError();
	log_msg("RESULT upload_dxt1 (%s): %.1f MB/s, %.1f ms per frame, glError 0x%04x",
	        sub ? "sub-image 800x480, 192000 B" : "full 1024x512, 262144 B",
	        mbps((f64)(sub ? part : full) * reps, ms), (f64)ms / reps, err);
	summary_add("upload dxt1%s %6.1f MB/s (%.1f ms/frame)",
	            sub ? "      " : "(full)", mbps((f64)(sub ? part : full) * reps, ms),
	            (f64)ms / reps);
	glDeleteTextures(1, &t);
}

static void draw_frame_quad(void)
{
	static const GLfloat v[8] = { 0, 0, W, 0, 0, H, W, H };
	/* texture row 0 is the frame's top line: flip t */
	static const GLfloat t[8] = {
		0, (GLfloat)H / TEX_H, (GLfloat)W / TEX_W, (GLfloat)H / TEX_H,
		0, 0, (GLfloat)W / TEX_W, 0,
	};

	glVertexPointer(2, GL_FLOAT, 0, v);
	glTexCoordPointer(2, GL_FLOAT, 0, t);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

enum { PIPE_RGB565_FULL, PIPE_RGB565_BAND, PIPE_DXT1_FULL };

/* Upload + draw + swap for PIPE_MS; logs fps. */
static void pipeline(struct gfx *g, int mode, u16 *const bars[2],
                     u8 *const dxt[2], const void *zero)
{
	static const char *const names[] = { "rgb565_full", "rgb565_band120", "dxt1_full" };
	GLuint t = new_texture();
	unsigned t0, ms, frames = 0, worst = 0, last;

	log_msg("gl: pipeline_%s: start", names[mode]);
	if (mode == PIPE_DXT1_FULL)
		glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
		                       TEX_W, TEX_H, 0, TEX_W * TEX_H / 2, zero);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX_W, TEX_H, 0, GL_RGB,
		             GL_UNSIGNED_SHORT_5_6_5, zero);
	if (mode == PIPE_RGB565_BAND)       /* start from a whole frame */
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGB,
		                GL_UNSIGNED_SHORT_5_6_5, bars[0]);

	t0 = last = log_ms();
	while (log_ms() - t0 < PIPE_MS && quit_poll() == QUIT_NONE) {
		int v = (int)(frames & 1);
		unsigned now;

		if (mode == PIPE_RGB565_FULL) {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGB,
			                GL_UNSIGNED_SHORT_5_6_5, bars[v]);
		} else if (mode == PIPE_RGB565_BAND) {
			int y = (int)(frames * 8 % (H - BAND_H));

			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, W, BAND_H, GL_RGB,
			                GL_UNSIGNED_SHORT_5_6_5, bars[v] + y * W);
		} else {
			glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H,
			                          GL_COMPRESSED_RGB_S3TC_DXT1_EXT, PIX / 2, dxt[v]);
		}
		glFinish();                    /* never two uploads in flight */
		draw_frame_quad();
		gfx_swap(g);
		if (frames == 0)
			log_msg("gl:   first frame ok");
		frames++;
		now = log_ms();
		if (now - last > worst)
			worst = now - last;
		last = now;
	}
	ms = log_ms() - t0;
	log_msg("RESULT pipeline_%s: %u frames in %u ms = %.1f fps, worst frame %u ms, "
	        "glError 0x%04x", names[mode], frames, ms, frames * 1000.0 / (ms ? ms : 1),
	        worst, glGetError());
	summary_add("pipeline %-15s %5.1f fps (worst %u ms)", names[mode],
	            frames * 1000.0 / (ms ? ms : 1), worst);
	glDeleteTextures(1, &t);
}

void bench_gpu(void)
{
	struct gfx g;
	u16 *bars[2] = { malloc(PIX * 2), malloc(PIX * 2) };
	u8 *dxt[2] = { malloc(PIX / 2), malloc(PIX / 2) };
	u8 *rgba[2] = { malloc(PIX * 4), malloc(PIX * 4) };
	void *zero = calloc(TEX_W * TEX_H, 4);
	int i, m;

	log_msg("== GPU / VFB benchmarks");
	if (!bars[0] || !bars[1] || !dxt[0] || !dxt[1] || !rgba[0] || !rgba[1] || !zero) {
		log_msg("FAIL out of memory for the GPU benchmarks");
		goto out;
	}
	for (i = 0; i < 2; i++) {
		frame_bars(bars[i], i * 5);
		frame_dxt1(dxt[i], i * 2);
		memset(rgba[i], 0x40 + 0x40 * i, PIX * 4);
	}

	memset(&g, 0, sizeof(g));
	if (gfx_init(&g, CID) != 0) {
		log_msg("FAIL gfx_init, no GPU benchmarks (see the lines above)");
		gfx_shutdown(&g);
		goto out;
	}
	bench_vfb(&g, rgba[0]);

	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0.0f, (GLfloat)W, 0.0f, (GLfloat)H, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_LIGHTING);
	glEnable(GL_TEXTURE_2D);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

	/* RGBA first: the only format a texture of ours has ever been created
	 * in without trouble (hello3/hello4, square, glTexImage2D only). */
	upload_case("rgba8888", GL_RGBA, GL_UNSIGNED_BYTE, 4, rgba[0], rgba[1], zero);
	upload_case("bgra8888", GL_BGRA_EXT, GL_UNSIGNED_BYTE, 4, rgba[0], rgba[1], zero);
	upload_case("rgb565", GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, bars[0], bars[1], zero);
	upload_dxt1(dxt[0], dxt[1], zero);
	for (m = PIPE_RGB565_FULL; m <= PIPE_DXT1_FULL && quit_poll() == QUIT_NONE; m++)
		pipeline(&g, m, bars, dxt, zero);

	gfx_shutdown(&g);
out:
	for (i = 0; i < 2; i++) {
		free(bars[i]);
		free(dxt[i]);
		free(rgba[i]);
	}
	free(zero);
}

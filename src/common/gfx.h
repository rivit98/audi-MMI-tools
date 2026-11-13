/*
 * gfx.h - bring up OpenGL ES (ES-CM 1.0 on this unit) on the MMI screen.
 *
 * Port of qnx-audi's Screen::init() (TARGET_AUDI path), same call order:
 * register a displayable with the layer manager, render through EGL into
 * its VFB surface via a gf 3D target on the topmost hardware layer. Every
 * step and its result is written to the log.
 */
#ifndef COMMON_GFX_H
#define COMMON_GFX_H

#include "types.h"
#include <EGL/egl.h>
#include <gf/gf3d.h>

#include "lmgr.h"

struct gfx {
	int cid;                 /* layer manager displayable id */
	int width, height;
	int layer_index;
	struct lm_vfb vfb;

	gf_dev_t dev;
	gf_display_t display;
	gf_layer_t layer;
	gf_surface_t surface;
	gf_3d_target_t target;

	EGLDisplay egl_display;
	EGLConfig egl_config;
	EGLContext egl_context;
	EGLSurface egl_surface;

	unsigned done;           /* GFX_STEP_* bits, for partial teardown */
};

/* Returns 0 when a GL context is current on the screen, -1 otherwise
 * (whatever was set up is still recorded in g->done for gfx_shutdown). */
int  gfx_init(struct gfx *g, int cid);

/* glFinish + eglWaitGL + eglSwapBuffers, as qnx-audi's main loop does. */
int  gfx_swap(struct gfx *g);

/* Hand the screen back to the HMI (qnx-audi's exit sequence) and free
 * everything gfx_init created. Safe after a partial gfx_init. */
void gfx_shutdown(struct gfx *g);

/* Linear fog over [start, end). Always use this instead of setting
 * GL_FOG_MODE / GL_FOG_START / GL_FOG_END by hand: while the mode is
 * GL_LINEAR this unit's nVidia driver recomputes end/(end-start) and
 * 1/(end-start) on every fog state change, with no guard for end == start,
 * and on QNX/SH a divide by zero is a fatal SIGFPE rather than an inf
 * (docs/qnx-sh-fpu-div0.md). start == end is clamped, not divided. */
void gfx_fog_linear(f32 start, f32 end);

const char *gf_format_name(gf_format_t f);
const char *gf_err_name(int err);
const char *egl_err_name(EGLint err);

#endif

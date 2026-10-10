/*
 * gfx.h - bring up OpenGL ES (ES-CM 1.0 on this unit) on the MMI screen.
 *
 * Port of qnx-audi's Screen::init() (TARGET_AUDI path), same call order:
 * register a displayable with the layer manager, render through EGL into
 * its VFB surface via a gf 3D target on the topmost hardware layer. Every
 * step and its result is written to the log.
 */
#ifndef RIVIT_GFX_H
#define RIVIT_GFX_H

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

const char *gf_format_name(gf_format_t f);
const char *gf_err_name(int err);
const char *egl_err_name(EGLint err);

#endif

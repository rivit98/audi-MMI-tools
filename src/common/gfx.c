#include "gfx.h"

#include <stdlib.h>
#include <string.h>

#include <GLES/gl.h>

#include "log.h"

enum {
	GFX_STEP_LM       = 1 << 0,
	GFX_STEP_DEV      = 1 << 1,
	GFX_STEP_DISPLAY  = 1 << 2,
	GFX_STEP_REGISTER = 1 << 3,
	GFX_STEP_SURFACE  = 1 << 4,
	GFX_STEP_LAYER    = 1 << 5,
	GFX_STEP_EGL      = 1 << 6,
	GFX_STEP_TARGET   = 1 << 7,
	GFX_STEP_CONTEXT  = 1 << 8,
	GFX_STEP_ESURFACE = 1 << 9,
	GFX_STEP_CURRENT  = 1 << 10,
	GFX_STEP_REG_TRY  = 1 << 11, /* register sent: unregister at shutdown */
};

/* Flags qnx-audi registers its displayable with (meaning unknown). */
#define LM_REGISTER_FLAGS 2

const char *gf_format_name(gf_format_t f)
{
	switch (f) {
	case GF_FORMAT_INVALID:       return "INVALID";
	case GF_FORMAT_BYTE:          return "BYTE";
	case GF_FORMAT_PAL8:          return "PAL8";
	case GF_FORMAT_PACK_ARGB1555: return "PACK_ARGB1555";
	case GF_FORMAT_PKLE_ARGB1555: return "PKLE_ARGB1555";
	case GF_FORMAT_PKBE_ARGB1555: return "PKBE_ARGB1555";
	case GF_FORMAT_PACK_RGB565:   return "PACK_RGB565";
	case GF_FORMAT_PKLE_RGB565:   return "PKLE_RGB565";
	case GF_FORMAT_PKBE_RGB565:   return "PKBE_RGB565";
	case GF_FORMAT_BGR888:        return "BGR888";
	case GF_FORMAT_BGRA8888:      return "BGRA8888";
	case GF_FORMAT_ARGB8888:      return "ARGB8888";
	case GF_FORMAT_PACK_YUV_UYVY: return "PACK_YUV_UYVY";
	case GF_FORMAT_PACK_YUV_YUY2: return "PACK_YUV_YUY2";
	case GF_FORMAT_PACK_YUV_NV12: return "PACK_YUV_NV12";
	case GF_FORMAT_PACK_YUV_AYUV: return "PACK_YUV_AYUV";
	case GF_FORMAT_PLANAR_YUV_420: return "PLANAR_YUV_420";
	default:                      return "?";
	}
}

const char *gf_err_name(int err)
{
	static const char *const names[] = {
		"GF_ERR_OK", "GF_ERR_MEM", "GF_ERR_IODISPLAY", "GF_ERR_DEVICE",
		"GF_ERR_SHMEM", "GF_ERR_DLL", "GF_ERR_THREAD", "GF_ERR_PARM",
		"GF_ERR_INUSE", "GF_ERR_NOSUPPORT", "GF_ERR_CFG",
	};

	return err >= 0 && err <= GF_ERR_CFG ? names[err] : "GF_ERR_?";
}

const char *egl_err_name(EGLint err)
{
	switch (err) {
	case EGL_SUCCESS:             return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED:     return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS:          return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC:           return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE:       return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONFIG:          return "EGL_BAD_CONFIG";
	case EGL_BAD_CONTEXT:         return "EGL_BAD_CONTEXT";
	case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
	case EGL_BAD_DISPLAY:         return "EGL_BAD_DISPLAY";
	case EGL_BAD_MATCH:           return "EGL_BAD_MATCH";
	case EGL_BAD_NATIVE_PIXMAP:   return "EGL_BAD_NATIVE_PIXMAP";
	case EGL_BAD_NATIVE_WINDOW:   return "EGL_BAD_NATIVE_WINDOW";
	case EGL_BAD_PARAMETER:       return "EGL_BAD_PARAMETER";
	case EGL_BAD_SURFACE:         return "EGL_BAD_SURFACE";
	case EGL_CONTEXT_LOST:        return "EGL_CONTEXT_LOST";
	default:                      return "EGL_?";
	}
}

static int fail_gf(const char *what, int rc)
{
	log_msg("FAIL %s: %d %s", what, rc, gf_err_name(rc));
	return -1;
}

static int fail_egl(const char *what)
{
	EGLint err = eglGetError();

	log_msg("FAIL %s: 0x%04x %s", what, err, egl_err_name(err));
	return -1;
}

static int fail_lm(const char *what, int rc)
{
	log_msg("FAIL %s: %d %s", what, rc, strerror(rc));
	return -1;
}

static void log_egl_configs(EGLDisplay dpy)
{
	EGLint n = 0, i;
	EGLConfig *cfgs;

	if (!eglGetConfigs(dpy, NULL, 0, &n) || n <= 0) {
		log_msg("eglGetConfigs: none (%s)", egl_err_name(eglGetError()));
		return;
	}
	cfgs = calloc(n, sizeof(*cfgs));
	if (cfgs == NULL || !eglGetConfigs(dpy, cfgs, n, &n)) {
		free(cfgs);
		return;
	}
	log_msg("EGL configs: %d  (id: rgba depth stencil samples surf_type visual caveat renderable)", n);
	for (i = 0; i < n; i++) {
		EGLint v[11];
		static const EGLint attr[11] = {
			EGL_CONFIG_ID, EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE,
			EGL_ALPHA_SIZE, EGL_DEPTH_SIZE, EGL_STENCIL_SIZE, EGL_SAMPLES,
			EGL_SURFACE_TYPE, EGL_NATIVE_VISUAL_ID, EGL_CONFIG_CAVEAT,
		};
		EGLint renderable = 0;
		int k;

		for (k = 0; k < 11; k++)
			if (!eglGetConfigAttrib(dpy, cfgs[i], attr[k], &v[k]))
				v[k] = -1;
		eglGetConfigAttrib(dpy, cfgs[i], EGL_RENDERABLE_TYPE, &renderable);
		log_msg("  cfg %2d: %d%d%d%d d%-2d s%d ms%d surf=0x%03x visual=%d(%s) caveat=0x%x rt=0x%x",
		        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8],
		        v[9], gf_format_name((gf_format_t)v[9]), v[10], renderable);
	}
	free(cfgs);
}

/* qnx-audi's searchForLayerAndEGLConfig(): walk the layer's pixel formats
 * and take the first EGL config whose native visual matches one. Logs
 * every layer format on the way. */
static int choose_config(struct gfx *g)
{
	/* The driver offers stencil on the 8888 configs (cfg 1/5/11/... in the
	 * log_egl_configs dump), but only if we ask: eglChooseConfig treats a
	 * missing EGL_STENCIL_SIZE as "0 is fine" and hands back the first
	 * match, which is a stencil-less config. Ask for 8 bits, and retry
	 * without if this layer format has no such config. */
	EGLint attribs[] = {
		EGL_NATIVE_VISUAL_ID, 0,
		EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 5, EGL_BLUE_SIZE, 5,
		EGL_DEPTH_SIZE, 16,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	const int stencil_val = 11;     /* index of the 8 above */
	gf_layer_info_t li;
	int i, found = 0;

	for (i = 0; i < 64; i++) {
		EGLint n = 0;
		int rc = gf_layer_query(g->layer, i, &li);

		if (rc != GF_ERR_OK) {
			log_msg("layer %d: %d format(s) (query %d -> %s)",
			        g->layer_index, i, i, gf_err_name(rc));
			break;
		}
		log_msg("layer %d fmt[%d]: %s (%d) caps=0x%x src_max=%dx%d dst_max=%dx%d",
		        g->layer_index, i, gf_format_name(li.format), li.format, li.caps,
		        li.src_max_width, li.src_max_height,
		        li.dst_max_width, li.dst_max_height);
		if (found)
			continue;
		attribs[1] = li.format;
		attribs[stencil_val] = 8;
		if (!(eglChooseConfig(g->egl_display, attribs, &g->egl_config, 1, &n) && n > 0)) {
			attribs[stencil_val] = 0;
			n = 0;
		}
		if (attribs[stencil_val] == 0 &&
		    !(eglChooseConfig(g->egl_display, attribs, &g->egl_config, 1, &n) && n > 0))
			n = 0;
		if (n > 0) {
			EGLint id = 0, sbits = 0, dbits = 0;

			eglGetConfigAttrib(g->egl_display, g->egl_config, EGL_CONFIG_ID, &id);
			eglGetConfigAttrib(g->egl_display, g->egl_config, EGL_STENCIL_SIZE, &sbits);
			eglGetConfigAttrib(g->egl_display, g->egl_config, EGL_DEPTH_SIZE, &dbits);
			log_msg("  -> chose EGL config %d for layer format %s (depth %d, stencil %d%s)",
			        id, gf_format_name(li.format), dbits, sbits,
			        attribs[stencil_val] == 0 ? ", no stencil config for this format" : "");
			found = 1;
		}
	}
	if (!found)
		log_msg("FAIL no EGL config matches any layer format");
	return found ? 0 : -1;
}

int gfx_init(struct gfx *g, int cid)
{
	gf_dev_info_t dev_info;
	gf_display_info_t disp_info;
	gf_surface_info_t si;
	EGLint major = 0, minor = 0, w = 0, h = 0;
	int rc, lm_major, lm_minor, lm_result;

	memset(g, 0, sizeof(*g));
	g->cid = cid;

	/* Layer manager */
	rc = lm_open();
	log_msg("lm_open /dev/layermanager: %s", rc ? strerror(rc) : "ok");
	if (rc)
		return -1;
	g->done |= GFX_STEP_LM;
	rc = lm_check_version(&lm_major, &lm_minor);
	log_msg("lm_check_version: rc=%d version=%d.%d (qnx-audi expects 1.1)",
	        rc, lm_major, lm_minor);

	/* gf device + display */
	rc = gf_dev_attach(&g->dev, GF_DEVICE_INDEX(0), &dev_info);
	if (rc != GF_ERR_OK)
		return fail_gf("gf_dev_attach", rc);
	g->done |= GFX_STEP_DEV;
	log_msg("gf_dev_attach: ndisplays=%u flags=0x%x description='%s'",
	        dev_info.ndisplays, dev_info.flags,
	        dev_info.description ? dev_info.description : "(null)");

	rc = gf_display_attach(&g->display, g->dev, 0, &disp_info);
	if (rc != GF_ERR_OK)
		return fail_gf("gf_display_attach", rc);
	g->done |= GFX_STEP_DISPLAY;
	g->width = disp_info.xres;
	g->height = disp_info.yres;
	g->layer_index = (int)disp_info.nlayers - 1; /* topmost, like qnx-audi */
	log_msg("gf_display_attach: %dx%d format=%s refresh=%d nlayers=%u main_layer=%u -> using layer %d",
	        g->width, g->height, gf_format_name(disp_info.format),
	        disp_info.refresh, disp_info.nlayers, disp_info.main_layer_index,
	        g->layer_index);

	/* Displayable + its virtual frame buffer. A displayable survives its
	 * client, so one left by a crashed run would make our register fail:
	 * unregister it first, like showScreen (EINVAL = it wasn't there). */
	rc = lm_unregister_displayable(cid);
	log_msg("lm_unregister_displayable(%d) before register: rc=%d %s",
	        cid, rc, rc ? strerror(rc) : "ok (a stale one was removed)");
	g->done |= GFX_STEP_REG_TRY;
	rc = lm_register_displayable(cid, g->width, g->height, LM_REGISTER_FLAGS,
	                             &lm_result);
	log_msg("lm_register_displayable(cid=%d, %dx%d, flags=%d): devctl rc=%d (%s), result=%d",
	        cid, g->width, g->height, LM_REGISTER_FLAGS, rc,
	        rc ? strerror(rc) : "ok", lm_result);
	if (rc != 0)
		return fail_lm("lm_register_displayable", rc);
	if (lm_result == 0) {
		log_msg("FAIL lm_register_displayable: devctl ok but result=0");
		return -1;
	}
	g->done |= GFX_STEP_REGISTER;
	rc = lm_get_vfb(cid, &g->vfb);
	log_msg("lm_get_vfb(cid=%d): rc=%d sid=%u uid=%u alpha_sid=%u",
	        cid, rc, g->vfb.sid, g->vfb.uid, g->vfb.alpha_sid);
	if (rc)
		return fail_lm("lm_get_vfb", rc);
	rc = gf_surface_attach_by_sid(&g->surface, g->dev, g->vfb.sid);
	if (rc != GF_ERR_OK)
		return fail_gf("gf_surface_attach_by_sid", rc);
	g->done |= GFX_STEP_SURFACE;
	memset(&si, 0, sizeof(si));
	gf_surface_get_info(g->surface, &si);
	log_msg("VFB surface: sid=%u %dx%d format=%s stride=%u vaddr=%p paddr=0x%llx flags=0x%x",
	        si.sid, si.w, si.h, gf_format_name(si.format), si.stride,
	        (void *)si.vaddr, (unsigned long long)si.paddr, si.flags);

	/* EGL display + layer */
	g->egl_display = eglGetDisplay((EGLNativeDisplayType)g->dev);
	if (g->egl_display == EGL_NO_DISPLAY)
		return fail_egl("eglGetDisplay");
	rc = gf_layer_attach(&g->layer, g->display, g->layer_index,
	                     GF_LAYER_ATTACH_PASSIVE);
	if (rc != GF_ERR_OK)
		return fail_gf("gf_layer_attach", rc);
	g->done |= GFX_STEP_LAYER;
	if (!eglInitialize(g->egl_display, &major, &minor))
		return fail_egl("eglInitialize");
	g->done |= GFX_STEP_EGL;
	log_msg("eglInitialize: EGL %d.%d", major, minor);
	log_msg("EGL_VENDOR: %s", eglQueryString(g->egl_display, EGL_VENDOR));
	log_msg("EGL_VERSION: %s", eglQueryString(g->egl_display, EGL_VERSION));
	log_msg("EGL_CLIENT_APIS: %s", eglQueryString(g->egl_display, EGL_CLIENT_APIS));
	log_msg("EGL_EXTENSIONS: %s", eglQueryString(g->egl_display, EGL_EXTENSIONS));
	log_egl_configs(g->egl_display);

	if (choose_config(g))
		return -1;

	/* 3D target rendering into the VFB surface, shown on the layer */
	rc = gf_3d_target_create(&g->target, g->layer, &g->surface, 1,
	                         g->width, g->height, si.format);
	if (rc != GF_ERR_OK)
		return fail_gf("gf_3d_target_create", rc);
	g->done |= GFX_STEP_TARGET;
	gf_layer_set_src_viewport(g->layer, 0, 0, g->width - 1, g->height - 1);
	gf_layer_set_dst_viewport(g->layer, 0, 0, g->width - 1, g->height - 1);
	gf_layer_enable(g->layer);
	log_msg("gf_3d_target_create + layer viewport/enable: ok");

	g->egl_context = eglCreateContext(g->egl_display, g->egl_config,
	                                  EGL_NO_CONTEXT, NULL);
	if (g->egl_context == EGL_NO_CONTEXT)
		return fail_egl("eglCreateContext");
	g->done |= GFX_STEP_CONTEXT;
	g->egl_surface = eglCreateWindowSurface(g->egl_display, g->egl_config,
	                                        (EGLNativeWindowType)g->target, NULL);
	if (g->egl_surface == EGL_NO_SURFACE)
		return fail_egl("eglCreateWindowSurface");
	g->done |= GFX_STEP_ESURFACE;
	if (!eglMakeCurrent(g->egl_display, g->egl_surface, g->egl_surface,
	                    g->egl_context))
		return fail_egl("eglMakeCurrent");
	g->done |= GFX_STEP_CURRENT;
	eglQuerySurface(g->egl_display, g->egl_surface, EGL_WIDTH, &w);
	eglQuerySurface(g->egl_display, g->egl_surface, EGL_HEIGHT, &h);
	log_msg("EGL context current, window surface %dx%d", w, h);
	return 0;
}

int gfx_swap(struct gfx *g)
{
	glFinish();
	eglWaitGL();
	return eglSwapBuffers(g->egl_display, g->egl_surface) ? 0 : -1;
}

/* See gfx.h: the driver divides by (end - start) whenever fog state changes
 * while GL_FOG_MODE is GL_LINEAR, so the mode is parked at GL_EXP (whose
 * recompute only divides by a constant) while the range is written. That
 * makes the degenerate state unreachable even when the new start equals the
 * old end, which is what killed hello4 at probe "fog". */
void gfx_fog_linear(float start, float end)
{
	if (end == start)
		end = start + 1.0f;     /* never hand the driver a zero divisor */

	glFogf(GL_FOG_MODE, GL_EXP);
	glFogf(GL_FOG_START, start);
	glFogf(GL_FOG_END, end);
	glFogf(GL_FOG_MODE, GL_LINEAR);
}

void gfx_shutdown(struct gfx *g)
{
	/* qnx-audi's exit: updateVfb, unregister, show displayables {cid,16,19}
	 * (showScreen's cleanup is the same minus updateVfb). Unregister also
	 * after a register that looked failed: the server may still hold it. */
	int cids[3] = { g->cid, 16, 19 };
	int rc;

	if (g->done & GFX_STEP_REGISTER) {
		rc = lm_update_vfb(g->cid);
		log_msg("lm_update_vfb(%d): %d", g->cid, rc);
	}
	if (g->done & GFX_STEP_REG_TRY) {
		rc = lm_unregister_displayable(g->cid);
		log_msg("lm_unregister_displayable(%d): %d", g->cid, rc);
		rc = lm_show_displayables(0, cids, 3);
		log_msg("lm_show_displayables(0, {%d,16,19}): %d", g->cid, rc);
	}

	if (g->done & GFX_STEP_CURRENT)
		eglMakeCurrent(g->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		               EGL_NO_CONTEXT);
	if (g->done & GFX_STEP_ESURFACE)
		eglDestroySurface(g->egl_display, g->egl_surface);
	if (g->done & GFX_STEP_CONTEXT)
		eglDestroyContext(g->egl_display, g->egl_context);
	if (g->done & GFX_STEP_TARGET)
		gf_3d_target_free(g->target);
	if (g->done & GFX_STEP_LAYER)
		gf_layer_detach(g->layer);
	if (g->done & GFX_STEP_EGL)
		eglTerminate(g->egl_display);
	if (g->done & GFX_STEP_SURFACE)
		gf_surface_free(g->surface);
	if (g->done & GFX_STEP_DISPLAY)
		gf_display_detach(g->display);
	if (g->done & GFX_STEP_DEV)
		gf_dev_detach(g->dev);
	if (g->done & GFX_STEP_LM)
		lm_close();
	log_msg("gfx shutdown done");
	g->done = 0;
}

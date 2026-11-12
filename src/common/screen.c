/*
 * screen.c - the layer manager displayable (see screen.h). Same sequence
 * as lmprobe (DrGER's showScreen), proven on the car: unregister, register
 * cid 40 flags 2, getVfb, attach the gf surface, draw, updateVfb, show
 * {40,16,19}; on exit unregister and show {40,16,19} again.
 *
 * The VFB is double-buffered by the layer manager (layermanagerV2,
 * CSurfacesManager): getVfb hands out the back surface, updateVfb makes it
 * the front one and the old front becomes the next VFB. So the sid changes
 * on every update and must be fetched again; drawing into the first sid
 * forever showed a never-written surface (noise) on every other update.
 */
#include <stdlib.h>
#include <string.h>

#include <gf/gf.h>

#include "lmgr.h"
#include "log.h"
#include "screen.h"
#include "vfbcopy.h"

#define CID            40
#define REGISTER_FLAGS 2
#define SCREEN_FULL_EVERY_MS 30000

static gf_dev_t dev;
static gf_display_t display;
/* the two surfaces the layer manager alternates between, attached lazily */
static struct vfb_slot {
	unsigned          sid;
	gf_surface_t      surface;
	gf_surface_info_t si;
	uint32_t         *shadow;   /* partial: what the surface holds */
	int               shadow_ok;
	unsigned          full_at;  /* log_ms() of its last full copy */
} slot[2];
static int cur;                 /* slot of the current VFB (back surface) */
static struct canvas back;
static int lm_is_open, registered, partial;
static struct screen_stats stats;
static unsigned reshow_ms, shown_at, reshows, reshow_fails;
static int shown;              /* screen_open's show is done */
static int no_show_on_close;

static int show_cid_and_hmi(void)
{
	int cids[3] = { CID, 16, 19 };
	int rc = lm_show_displayables(0, cids, 3);

	shown = 1;
	shown_at = log_ms();
	return rc;
}

static void show_logged(void)
{
	int rc = show_cid_and_hmi();

	log_msg("  lm_show_displayables(0, {%d,16,19}): rc=%d %s", CID, rc,
	        rc ? strerror(rc) : "ok");
}

/* screen_set_reshow(): show again once reshow_ms have passed. The first
 * one and every failure are logged, the rest only counted. */
static void maybe_reshow(void)
{
	int rc;

	if (reshow_ms == 0 || !registered || !shown || log_ms() - shown_at < reshow_ms)
		return;
	rc = show_cid_and_hmi();
	reshows++;
	if (rc != 0)
		reshow_fails++;
	if (rc != 0 || reshows == 1)
		log_msg("screen: re-show %u (every %u ms): rc=%d %s", reshows,
		        reshow_ms, rc, rc ? strerror(rc) : "ok");
}

/* The surface goes; its shadow buffer is kept for the next one. */
static void free_slot(int i)
{
	uint32_t *shadow = slot[i].shadow;

	if (slot[i].surface != NULL)
		gf_surface_free(slot[i].surface);
	memset(&slot[i], 0, sizeof(slot[i]));
	slot[i].shadow = shadow;
}

/* Attach the gf surface sid into slot i (replacing what was there) and
 * check that it has a usable 32-bit CPU mapping. */
static int attach_slot(int i, unsigned sid)
{
	struct vfb_slot *v = &slot[i];
	int rc;

	free_slot(i);
	rc = gf_surface_attach_by_sid(&v->surface, dev, sid);
	log_msg("  gf_surface_attach_by_sid(%u): %d", sid, rc);
	if (rc != GF_ERR_OK) {
		v->surface = NULL;
		return -1;
	}
	v->sid = sid;
	gf_surface_get_info(v->surface, &v->si);
	log_msg("  VFB slot %d: sid %u %dx%d format=%d stride=%u vaddr=%p", i, sid,
	        v->si.w, v->si.h, v->si.format, v->si.stride, (void *)v->si.vaddr);
	if (v->si.vaddr == NULL || v->si.w <= 0 || v->si.h <= 0 ||
	    v->si.stride < (unsigned)v->si.w * 4) {
		log_msg("FAIL no 32-bit CPU mapping of the VFB");
		return -1;
	}
	return 0;
}

int screen_open(void)
{
	gf_dev_info_t dev_info;
	gf_display_info_t disp_info;
	struct lm_vfb vfb;
	const gf_surface_info_t *si;
	int rc, result = 0, major = 0, minor = 0;

	log_msg("screen_open:");
	rc = gf_dev_attach(&dev, GF_DEVICE_INDEX(0), &dev_info);
	log_msg("  gf_dev_attach: %d", rc);
	if (rc != GF_ERR_OK)
		return -1;
	rc = gf_display_attach(&display, dev, 0, &disp_info);
	log_msg("  gf_display_attach: %d  %dx%d", rc, disp_info.xres, disp_info.yres);
	if (rc != GF_ERR_OK)
		return -1;

	rc = lm_open();
	log_msg("  lm_open: %s", rc ? strerror(rc) : "ok");
	if (rc)
		return -1;
	lm_is_open = 1;
	rc = lm_check_version(&major, &minor);
	log_msg("  lm_check_version: rc=%d version=%d.%d", rc, major, minor);

	rc = lm_unregister_displayable(CID);   /* EINVAL = was not registered */
	log_msg("  lm_unregister_displayable(%d): rc=%d", CID, rc);
	rc = lm_register_displayable(CID, disp_info.xres, disp_info.yres,
	                             REGISTER_FLAGS, &result);
	log_msg("  lm_register_displayable(%d, %dx%d, %d): rc=%d result=%d",
	        CID, disp_info.xres, disp_info.yres, REGISTER_FLAGS, rc, result);
	if (rc != 0 || result == 0)
		return -1;
	registered = 1;

	rc = lm_get_vfb(CID, &vfb);
	log_msg("  lm_get_vfb(%d): rc=%d sid=%u", CID, rc, vfb.sid);
	if (rc)
		return -1;
	cur = 0;
	if (attach_slot(0, vfb.sid) != 0)
		return -1;
	si = &slot[0].si;

	back.w = si->w;
	back.h = si->h;
	back.stride = si->w;
	back.px = calloc((size_t)si->w * si->h, 4);
	if (back.px == NULL) {
		log_msg("FAIL out of memory for the back buffer");
		return -1;
	}
	screen_present();
	show_logged();
	return 0;
}

struct canvas *screen_canvas(void)
{
	return &back;
}

void screen_set_partial(int on)
{
	partial = on;
}

void screen_set_reshow(unsigned ms)
{
	reshow_ms = ms;
}

const struct screen_stats *screen_last_stats(void)
{
	return &stats;
}

/* back -> the VFB of slot v; all of it, or (partial) only what differs
 * from its shadow. Fills stats.bytes/rows/full. */
static void copy_to_vfb(struct vfb_slot *v)
{
	int w = back.w < v->si.w ? back.w : v->si.w;
	int h = back.h < v->si.h ? back.h : v->si.h;
	unsigned now = log_ms();
	struct vfb_copied c;
	int full;

	if (partial && v->shadow == NULL)
		v->shadow = malloc((size_t)back.w * back.h * 4);
	full = !partial || v->shadow == NULL || !v->shadow_ok ||
	       now - v->full_at >= SCREEN_FULL_EVERY_MS;
	/* the shadow is w pixels a row: only usable when the VFB is the
	 * canvas's size (it always is: both come from the same surface) */
	if (w != back.w)
		full = 1;
	c = vfb_copy(v->si.vaddr, v->si.stride, partial ? v->shadow : NULL, back.px,
	             back.stride, w, h, full);
	stats.full = full;
	stats.bytes = c.bytes;
	stats.rows = c.rows;
	if (full && v->shadow != NULL && w == back.w) {
		v->shadow_ok = 1;
		v->full_at = now;
	}
}

/* Hand the VFB to the layer manager and pick up the next one. */
static int screen_flip(void)
{
	struct lm_vfb vfb;
	int rc;

	rc = lm_update_vfb(CID);
	if (rc)
		return rc;
	/* the layer manager flipped its surfaces: the next VFB is the other one */
	rc = lm_get_vfb(CID, &vfb);
	if (rc) {
		log_msg("  lm_get_vfb(%d) after update: rc=%d", CID, rc);
		return rc;
	}
	if (vfb.sid == slot[cur].sid)
		return 0;
	if (vfb.sid == slot[!cur].sid && slot[!cur].surface != NULL) {
		cur = !cur;
		return 0;
	}
	cur = !cur;
	return attach_slot(cur, vfb.sid) ? -1 : 0;
}

int screen_present(void)
{
	struct vfb_slot *v = &slot[cur];
	unsigned t0;
	int rc;

	if (back.px == NULL || v->surface == NULL)
		return -1;
	t0 = log_us();
	copy_to_vfb(v);
	stats.copy_us = log_us() - t0;
	t0 = log_us();
	rc = screen_flip();
	stats.flip_us = log_us() - t0;
	maybe_reshow();
	return rc;
}

void screen_show_again(void)
{
	if (lm_is_open && registered)
		show_logged();
}

void screen_give_back_to_camera(void)
{
	int cids[2] = { 0x10, 0x11 }; /* HMI, rear camera: docs/rear-camera.md 6 */

	if (lm_is_open)
		log_msg("  lm_show_displayables(0, {16,17}): rc=%d", lm_show_displayables(0, cids, 2));
}

void screen_no_show_on_close(void)
{
	no_show_on_close = 1;
}

void screen_close(void)
{
	int rc;

	log_msg("screen_close:");
	if (!lm_is_open && lm_open() == 0)
		lm_is_open = 1;
	if (lm_is_open) {
		rc = lm_unregister_displayable(CID);
		log_msg("  lm_unregister_displayable(%d): rc=%d%s", CID, rc,
		        registered ? "" : " (was not registered)");
		if (reshows != 0)
			log_msg("  re-shows: %u, %u failed", reshows, reshow_fails);
		if (no_show_on_close)
			log_msg("  no show: the rear camera is up");
		else
			show_logged();
		no_show_on_close = 0;
		lm_close();
		shown = 0;
		reshows = reshow_fails = 0;
		lm_is_open = 0;
	}
	registered = 0;
	free_slot(0);
	free_slot(1);
	free(slot[0].shadow);
	free(slot[1].shadow);
	slot[0].shadow = slot[1].shadow = NULL;
	if (display != NULL)
		gf_display_detach(display);
	if (dev != NULL)
		gf_dev_detach(dev);
	display = NULL;
	dev = NULL;
	free(back.px);
	back.px = NULL;
}

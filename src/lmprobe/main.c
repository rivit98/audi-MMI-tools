/*
 * lmprobe - layer manager control test, no OpenGL.
 *
 * Replays DrGER's showScreen call for call (RE of its 0x08041b34 setup and
 * 0x08041d04 cleanup), but fills the displayable's frame buffer with colour
 * bars instead of a PNG, and logs every raw devctl status:
 *
 *   cleanup   connect, unregister 40, show {40,16,19}, disconnect
 *   setup     gf_dev_attach, gf_display_attach, connect (open + version),
 *             register 40, getVfb, draw into the VFB, updateVfb,
 *             show {40,16,19}, disconnect
 *   wait      <seconds>
 *   cleanup   as above
 *
 * Around those steps it reads the layer manager's state dump (a read() of
 * /dev/layermanager, lmstate.h): before, while the bars are shown, and
 * after. Each dump goes to a file, its layers and displayables to the log,
 * and the foreground (layer 0) displayable after must be the one before.
 * After updateVfb it asks getVfb again: the VFB is double-buffered, so the
 * sid should change.
 *
 * Writes <sd>/var/lmprobe.log and lmprobe_state_<when>.txt.
 *
 * usage: lmprobe <sd_path> [seconds]
 */
#include <errno.h>
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gf/gf.h>

#include "lmgr.h"
#include "lmstate.h"
#include "log.h"
#include "quit.h"

#define CID            40   /* showScreen's displayable id */
#define REGISTER_FLAGS 2    /* showScreen's flags */
#define RUN_SECONDS    5
#define WATCHDOG_SLACK 30

static const char *sd_path;
static const char *out_dir;    /* <sd_path>/var: everything written */

#define STATE_SIZE  65536
#define STATE_MAX   64          /* layers / displayables logged */
static char state[STATE_SIZE];

/* Reads the state dump, saves it as lmprobe_state_<when>.txt and logs what
 * it says. Returns the foreground (layer 0) cid, or -1. */
static int dump_state(const char *when)
{
	struct lm_state_layer layers[STATE_MAX];
	struct lm_state_displayable ds[STATE_MAX];
	char path[300];
	usize len;
	isize second;
	int rc, nl, nd, i, fg;
	FILE *f;

	rc = lm_read_state(state, sizeof(state), &len, &second);
	log_msg("state (%s): read rc=%d %s, %u bytes, second read %d",
	        when, rc, rc ? strerror(rc) : "ok", (unsigned)len, (int)second);
	if (len == 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lmprobe_state_%s.txt", out_dir, when);
	f = fopen(path, "w");
	if (f != NULL) {
		fwrite(state, 1, len, f);
		fclose(f);
	}
	nl = lm_state_layers(state, layers, STATE_MAX);
	nd = lm_state_displayables(state, ds, STATE_MAX);
	fg = lm_state_layer_cid(state, 0);
	log_msg("  %s, %d layer lines, %d displayables, foreground (layer 0) cid %d",
	        lm_state_unlocked(state) ? "unlocked" : "LOCKED or no lock line", nl, nd, fg);
	for (i = 0; i < nl && i < STATE_MAX; i++)
		log_msg("  layer %d gf %d -> cid %d", layers[i].layer, layers[i].gf_index,
		        layers[i].cid);
	for (i = 0; i < nd && i < STATE_MAX; i++)
		log_msg("  cid %d: front uid %d sid %d, back uid %d sid %d, visible gf %d display %d",
		        ds[i].cid, ds[i].front_uid, ds[i].front_sid, ds[i].back_uid,
		        ds[i].back_sid, ds[i].visible_gf_index, ds[i].visible_display);
	return fg;
}

static void lm_connect(void)
{
	int rc, major = 0, minor = 0;

	rc = lm_open();
	log_msg("  lm_open: %s", rc ? strerror(rc) : "ok");
	if (rc)
		return;
	rc = lm_check_version(&major, &minor);
	log_msg("  lm_check_version: rc=%d version=%d.%d", rc, major, minor);
}

static void show_cid_and_hmi(void)
{
	int cids[3] = { CID, 16, 19 };
	int rc = lm_show_displayables(0, cids, 3);

	log_msg("  lm_show_displayables(0, {%d,16,19}): rc=%d %s", CID, rc,
	        rc ? strerror(rc) : "ok");
}

/* showScreen 0x08041d04 */
static void cleanup(const char *when)
{
	int rc;

	log_msg("cleanup (%s):", when);
	lm_connect();
	rc = lm_unregister_displayable(CID);
	log_msg("  lm_unregister_displayable(%d): rc=%d %s", CID, rc,
	        rc ? strerror(rc) : "ok (it was registered)");
	show_cid_and_hmi();
	lm_close();
}

/* Vertical colour bars, 0xAARRGGBB (BGRA8888 in memory, little endian). */
static void fill_bars(const gf_surface_info_t *si)
{
	static const u32 bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	int x, y;

	for (y = 0; y < si->h; y++) {
		u32 *row = (u32 *)(si->vaddr + (unsigned)y * si->stride);

		for (x = 0; x < si->w; x++)
			row[x] = bars[x * 8 / si->w];
	}
}

/* showScreen 0x08041b34. Returns 0 if the bars were shown. */
static int setup(gf_dev_t *dev, gf_display_t *display, gf_surface_t *surface)
{
	gf_dev_info_t dev_info;
	gf_display_info_t disp_info;
	gf_surface_info_t si;
	struct lm_vfb vfb;
	int rc, result = 0, w, h;

	log_msg("setup:");
	rc = gf_dev_attach(dev, GF_DEVICE_INDEX(0), &dev_info);
	log_msg("  gf_dev_attach: %d", rc);
	if (rc != GF_ERR_OK)
		return -1;
	rc = gf_display_attach(display, *dev, 0, &disp_info);
	log_msg("  gf_display_attach: %d  %dx%d", rc, disp_info.xres, disp_info.yres);
	if (rc != GF_ERR_OK)
		return -1;
	w = disp_info.xres;
	h = disp_info.yres;

	lm_connect();
	rc = lm_register_displayable(CID, w, h, REGISTER_FLAGS, &result);
	log_msg("  lm_register_displayable(cid=%d, %dx%d, flags=%d): devctl rc=%d (%s), result=%d",
	        CID, w, h, REGISTER_FLAGS, rc, rc ? strerror(rc) : "ok", result);
	if (rc != 0 || result == 0) {
		log_msg("FAIL register");
		lm_close();
		return -1;
	}

	rc = lm_get_vfb(CID, &vfb);
	log_msg("  lm_get_vfb(%d): rc=%d sid=%u uid=%u alpha_sid=%u",
	        CID, rc, vfb.sid, vfb.uid, vfb.alpha_sid);
	if (rc) {
		log_msg("FAIL get_vfb");
		lm_close();
		return -1;
	}
	rc = gf_surface_attach_by_sid(surface, *dev, vfb.sid);
	log_msg("  gf_surface_attach_by_sid(%u): %d", vfb.sid, rc);
	if (rc != GF_ERR_OK) {
		*surface = NULL;
		lm_close();
		return -1;
	}
	memset(&si, 0, sizeof(si));
	gf_surface_get_info(*surface, &si);
	log_msg("  VFB: %dx%d format=%d stride=%u vaddr=%p",
	        si.w, si.h, si.format, si.stride, (void *)si.vaddr);
	if (si.vaddr != NULL && si.stride >= (unsigned)si.w * 4) {
		fill_bars(&si);
		log_msg("  filled colour bars");
	} else {
		log_msg("  FAIL no 32-bit CPU mapping, VFB left as is");
	}

	rc = lm_update_vfb(CID);
	log_msg("  lm_update_vfb(%d): rc=%d %s", CID, rc, rc ? strerror(rc) : "ok");
	{
		struct lm_vfb next;
		int rc2 = lm_get_vfb(CID, &next);

		log_msg("  lm_get_vfb(%d) after update: rc=%d sid=%u (was %u: %s)", CID, rc2,
		        next.sid, vfb.sid, next.sid != vfb.sid ? "double-buffered" : "same");
	}
	show_cid_and_hmi();
	lm_close();
	return 0;
}

/* quit_finish() or the hard path: the screen back */
static void on_quit(int hard, void *arg)
{
	(void)hard;
	(void)arg;
	cleanup("after");
}

int main(int argc, char *argv[])
{
	struct quit_config qc = { 0 };
	gf_dev_t dev = NULL;
	gf_display_t display = NULL;
	gf_surface_t surface = NULL;
	unsigned seconds = RUN_SECONDS;
	char path[256];
	int shown, fg_before, fg_after;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <sd_path> [seconds]\n", argv[0]);
		return 2;
	}
	sd_path = argv[1];
	out_dir = var_dir(sd_path);
	if (argc > 2 && atoi(argv[2]) > 0)
		seconds = (unsigned)atoi(argv[2]);

	snprintf(path, sizeof(path), "%s/lmprobe.log", out_dir);
	if (log_open(path) != 0)
		fprintf(stderr, "lmprobe: cannot open %s, logging to stdout only\n", path);
	log_msg("lmprobe start, pid=%d, show=%u s", getpid(), seconds);
	qc.sd_path = sd_path;
	qc.run_ms = seconds * 1000;
	qc.watchdog_s = seconds + WATCHDOG_SLACK;
	qc.cleanup = on_quit;
	quit_start(&qc);

	fg_before = dump_state("before");
	cleanup("before");
	shown = setup(&dev, &display, &surface) == 0;
	dump_state("shown");
	if (shown) {
		log_msg("bars shown, waiting %u s", seconds);
		while (quit_poll() == QUIT_NONE)
			usleep(100 * 1000);
	}
	quit_finish();
	fg_after = dump_state("after");
	log_msg("foreground before %d, after %d: %s", fg_before, fg_after,
	        fg_before == fg_after ? "restored" : "CHANGED");

	if (surface != NULL)
		gf_surface_free(surface);
	if (display != NULL)
		gf_display_detach(display);
	if (dev != NULL)
		gf_dev_detach(dev);
	log_msg("lmprobe exit: %s (stop reason: %s)", shown ? "ok" : "FAILED",
	        quit_name(quit_poll()));
	log_close();
	return shown ? 0 : 1;
}

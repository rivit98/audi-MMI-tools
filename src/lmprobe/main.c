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
 * Writes <sd>/lmprobe.log, lmprobe_dshot.png (our VFB) and
 * lmprobe_sshot.png (whole display).
 *
 * usage: lmprobe <sd_path> [seconds]
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gf/gf.h>

#include "lmgr.h"
#include "log.h"
#include "quit.h"

#define CID            40   /* showScreen's displayable id */
#define REGISTER_FLAGS 2    /* showScreen's flags */
#define RUN_SECONDS    5
#define WATCHDOG_SLACK 30

static const char *sd_path;
static const char *out_dir;    /* <sd_path>/var: everything written */

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
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	int x, y;

	for (y = 0; y < si->h; y++) {
		uint32_t *row = (uint32_t *)(si->vaddr + (unsigned)y * si->stride);

		for (x = 0; x < si->w; x++)
			row[x] = bars[x * 8 / si->w];
	}
}

static void screenshots(void)
{
	char cmd[300];
	int rc;

	snprintf(cmd, sizeof(cmd), "d-shot %d %s/lmprobe_dshot.png", CID, out_dir);
	rc = lm_text_command(cmd);
	log_msg("  layermanager '%s': %s", cmd, rc ? strerror(rc) : "ok");
	snprintf(cmd, sizeof(cmd), "s-shot 0 %s/lmprobe_sshot.png", out_dir);
	rc = lm_text_command(cmd);
	log_msg("  layermanager '%s': %s", cmd, rc ? strerror(rc) : "ok");
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
	show_cid_and_hmi();
	screenshots();
	lm_close();
	return 0;
}

int main(int argc, char *argv[])
{
	gf_dev_t dev = NULL;
	gf_display_t display = NULL;
	gf_surface_t surface = NULL;
	unsigned seconds = RUN_SECONDS, start;
	char path[256];
	int shown;

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
	quit_init(sd_path, seconds + WATCHDOG_SLACK);

	cleanup("before");
	shown = setup(&dev, &display, &surface) == 0;
	if (shown) {
		log_msg("bars shown, waiting %u s", seconds);
		start = log_ms();
		while (quit_poll() == QUIT_NONE) {
			if (log_ms() - start >= seconds * 1000)
				quit_request(QUIT_TIMER);
			usleep(100 * 1000);
		}
	}
	cleanup("after");

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

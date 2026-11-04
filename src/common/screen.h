/*
 * screen.h - full-screen displayable through the layer manager, drawn by
 * the CPU (no GL): the lmprobe sequence, kept open for the whole run.
 *
 * Draw into screen_canvas() (an off-screen buffer), then screen_present()
 * copies it into the VFB and tells the layer manager.
 *
 * CPU writes into the VFB are slow (~9 MB/s: a full 800x480 copy takes
 * ~160 ms, docs/gpu-results.md). With screen_set_partial(1), present
 * keeps a RAM copy of what each of the two VFB surfaces holds and writes
 * only the changed part of each row (first to last changed pixel). A
 * surface gets a full copy when it is new and every SCREEN_FULL_EVERY_MS,
 * in case something else wrote into it.
 */
#ifndef COMMON_SCREEN_H
#define COMMON_SCREEN_H

#include "text.h"

/* 0 if the displayable is up and shown. Logs every step. */
int  screen_open(void);
struct canvas *screen_canvas(void);
int  screen_present(void);

/* Off by default (costs two screen-sized RAM buffers). */
void screen_set_partial(int on);

/* Off by default (0). With ms > 0, screen_present() shows our displayable
 * again (show {40,16,19}) when the last show is ms or more ago. The HMI
 * takes the screen with its own show on some keys (MEDIA in hello5 on
 * 2026-11-03, MENU/BACK in hello7 on 2026-10-25) and the layer manager
 * offers no way to see that, so an app that must stay up re-shows on a
 * timer. */
void screen_set_reshow(unsigned ms);

/* What the last screen_present() did. */
struct screen_stats {
	unsigned copy_us;   /* compare + copy into the VFB */
	unsigned flip_us;   /* lm_update_vfb + lm_get_vfb */
	unsigned bytes;     /* written into the VFB */
	unsigned rows;      /* rows with something written */
	int      full;      /* a full copy (new surface / refresh / partial off) */
};
const struct screen_stats *screen_last_stats(void);
/* Unregister our displayable and give the screen back to the HMI. Safe to
 * call when screen_open() failed. */
void screen_close(void);

#endif

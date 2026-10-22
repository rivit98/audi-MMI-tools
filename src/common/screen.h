/*
 * screen.h - full-screen displayable through the layer manager, drawn by
 * the CPU (no GL): the lmprobe sequence, kept open for the whole run.
 *
 * Draw into screen_canvas() (an off-screen buffer), then screen_present()
 * copies it into the VFB and tells the layer manager.
 */
#ifndef COMMON_SCREEN_H
#define COMMON_SCREEN_H

#include "text.h"

/* 0 if the displayable is up and shown. Logs every step. */
int  screen_open(void);
struct canvas *screen_canvas(void);
int  screen_present(void);
/* Unregister our displayable and give the screen back to the HMI. Safe to
 * call when screen_open() failed. */
void screen_close(void);

#endif

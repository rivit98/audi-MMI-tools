/*
 * ui.h - draws the whole usb_tx_test screen from the model.
 */
#ifndef USBTX_UI_H
#define USBTX_UI_H

#include "text.h"

/* Redraw everything into c (locks the model while reading it). */
void ui_render(struct canvas *c, const char *sd_path);

#endif

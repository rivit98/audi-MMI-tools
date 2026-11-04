/*
 * ui.h - draws the whole audi_stream_receiver screen from the model and the phone's
 * picture (video.h). Main loop only.
 */
#ifndef AUDI_STREAM_UI_H
#define AUDI_STREAM_UI_H

#include "text.h"

/* Redraw everything into c (locks the model while reading it). */
void ui_render(struct canvas *c, const char *sd_path);

#endif

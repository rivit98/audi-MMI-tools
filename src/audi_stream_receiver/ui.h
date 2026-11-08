// ui.h - draws the whole audi_stream_receiver screen from the model and the phone's
// picture (video.h). Main loop only.
#ifndef AUDI_STREAM_UI_H
#define AUDI_STREAM_UI_H

extern "C" {
#include "text.h"
}

namespace ui {

// Redraw everything into c.
void render(canvas *c, const char *sd_path);

} // namespace ui

#endif

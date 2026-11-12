// ui.h - draws the whole audi_stream_receiver screen from the model and the phone's
// picture (video.h). Main loop only.
#ifndef AUDI_STREAM_UI_H
#define AUDI_STREAM_UI_H

extern "C" {
#include "text.h"
}

namespace ui {

// Redraw everything into c. out_dir: where the log is (<sd>/var), shown.
void render(canvas *c, const char *out_dir);

} // namespace ui

#endif

// ui.h - full-screen text screens for the test apps (hello9, hello10):
// cid 40, CPU-drawn through common/screen.c, as hello6's label.c.
#ifndef COMMON_UI_H
#define COMMON_UI_H

#include <string>
#include <vector>

namespace ui {

// Title, word-wrapped body, small footer. 0 if shown.
int show(const char *title, const std::string &body, const char *footer);
// Title + lines in the small font (the summary). Returns how many lines did
// not fit.
int show_list(const char *title, const std::vector<std::string> &lines);
// Unregister the screen and give it back to the HMI.
void hide();

} // namespace ui

#endif

// menu.h - the runner's menu: a list moved by the main knob and started by
// its press (buttons.h events). Pure, tested on the host
// (tests/test_runner.cpp).
#ifndef RUNNER_MENU_H
#define RUNNER_MENU_H

#include "buttons.h"
#include "types.h"

namespace runner {

class Menu {
public:
	enum class Act { None, Moved, Start };

	// count items; rows of them fit on the screen.
	Menu(int count, int rows);

	// Main knob turn: clockwise moves down, by the detents, wrapping
	// around at either end (past the last item comes the first). Main knob
	// press: Start the selected item. Anything else:
	// None.
	Act on_event(const buttons::Event &e);

	int selected() const { return sel_; }
	void select(int i);
	// The first item on screen: the window follows the selection.
	int top() const { return top_; }
	int count() const { return count_; }

private:
	void follow();

	int count_, rows_, sel_ = 0, top_ = 0;
};

} // namespace runner

#endif

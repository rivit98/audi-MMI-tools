// menu.cpp - see menu.h.
#include "menu.h"

namespace runner {

Menu::Menu(int count, int rows)
	: count_(count > 0 ? count : 0), rows_(rows > 0 ? rows : 1)
{
}

void Menu::select(int i)
{
	if (count_ == 0)
		return;
	sel_ = i < 0 ? 0 : i >= count_ ? count_ - 1 : i;
	follow();
}

void Menu::follow()
{
	if (sel_ < top_)
		top_ = sel_;
	else if (sel_ >= top_ + rows_)
		top_ = sel_ - rows_ + 1;
}

Menu::Act Menu::on_event(const buttons::Event &e)
{
	if (count_ == 0 || e.key != buttons::Key::MainKnob)
		return Act::None;
	if (e.action == buttons::Action::Press)
		return Act::Start;
	if (e.action != buttons::Action::Turn)
		return Act::None;
	int was = sel_;
	select(sel_ + e.detents);
	return sel_ != was ? Act::Moved : Act::None;
}

} // namespace runner

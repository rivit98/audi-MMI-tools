// buttons.cpp - see buttons.h.
#include "buttons.h"

#include <cstdio>

namespace buttons {

namespace {

// Every button bit named on the car (README.md, "The table").
struct Bit {
	u8 byte, mask;
	Key key;
};
constexpr Bit kBits[] = {
	{0, 0x01, Key::Radio},
	{0, 0x02, Key::Media},
	{0, 0x08, Key::Tel},
	{0, 0x10, Key::Nav},
	{0, 0x40, Key::Car},
	{1, 0x01, Key::Next},
	{1, 0x02, Key::SoftTopLeft},
	{1, 0x04, Key::SoftBottomLeft},
	{1, 0x08, Key::Prev},
	{1, 0x10, Key::SoftTopRight},
	{1, 0x20, Key::SoftBottomRight},
	{1, 0x40, Key::Back},
	{1, 0x80, Key::MainKnob},
	{2, 0x01, Key::VolumeKnob},
	{3, 0x04, Key::Menu},
};

// The knob bytes: detents since the last message, never button bits.
struct Knob {
	u8 byte;
	Key key;
	i32 sign;
};
constexpr Knob kKnobs[] = {
	{6, Key::MainKnob, +1},
	{7, Key::MainKnob, -1},
	{4, Key::VolumeKnob, +1},
	{5, Key::VolumeKnob, -1},
};

bool is_knob_byte(int i)
{
	return i >= 4 && i <= 7;
}

Event button(Key k, Action a, int byte, int mask, u32 ms)
{
	Event e;
	e.key = k;
	e.action = a;
	e.byte = (u8)byte;
	e.mask = (u8)mask;
	e.ms = ms;
	return e;
}

} // namespace

Key key_of(int byte, int mask)
{
	for (const Bit &b : kBits)
		if (b.byte == byte && b.mask == mask)
			return b.key;
	return Key::Unknown;
}

void Decoder::reset()
{
	for (u8 &h : held_)
		h = 0;
	power_ = 0;
}

void Decoder::decode(const u8 *buf, int n, u32 ms, std::vector<Event> &out)
{
	if (n <= kPowerByte)
		return;
	bool touch = buf[kKindByte] == 0x02;

	// Buttons: the change against the held state, releases first. A touch
	// pad message carries no button state (its control bytes are zero even
	// while a button is held), so it doesn't release anything.
	if (!touch) {
		for (int i = 0; i < kControlBytes; i++) {
			if (is_knob_byte(i))
				continue;
			u8 now = buf[i], was = held_[i];
			for (int bit = 0; bit < 8; bit++) {
				int m = 1 << bit;
				if ((was & m) && !(now & m))
					out.push_back(button(key_of(i, m), Action::Release, i, m, ms));
			}
			for (int bit = 0; bit < 8; bit++) {
				int m = 1 << bit;
				if (!(was & m) && (now & m))
					out.push_back(button(key_of(i, m), Action::Press, i, m, ms));
			}
			held_[i] = now;
		}
	}

	for (const Knob &k : kKnobs) {
		if (buf[k.byte] == 0)
			continue;
		Event e;
		e.key = k.key;
		e.action = Action::Turn;
		e.detents = k.sign * (i32)buf[k.byte];
		e.byte = k.byte;
		e.ms = ms;
		out.push_back(e);
	}

	if (touch && n > kMsgBytes) {
		Event e;
		e.key = Key::TouchPad;
		e.action = Action::Touch;
		e.ms = ms;
		for (int i = kMsgBytes; i < n && e.touch_len < kTouchMax; i++)
			e.touch[e.touch_len++] = buf[i];
		out.push_back(e);
	}

	u8 p = buf[kPowerByte];
	if (p != power_) {
		// The first message only sets the baseline, unless the unit is
		// already on the way down.
		if (power_ != 0 || p != 0x02) {
			Event e;
			e.action = Action::Power;
			e.power = p;
			e.ms = ms;
			out.push_back(e);
		}
		power_ = p;
	}
}

const char *key_name(Key k)
{
	switch (k) {
	case Key::None:            return "none";
	case Key::MainKnob:        return "main knob";
	case Key::VolumeKnob:      return "volume knob";
	case Key::Radio:           return "RADIO";
	case Key::Media:           return "MEDIA";
	case Key::Tel:             return "TEL";
	case Key::Nav:             return "NAV";
	case Key::Car:             return "CAR";
	case Key::Next:            return "next track";
	case Key::Prev:            return "previous track";
	case Key::SoftTopLeft:     return "soft key top left";
	case Key::SoftBottomLeft:  return "soft key bottom left";
	case Key::SoftTopRight:    return "soft key top right";
	case Key::SoftBottomRight: return "soft key bottom right";
	case Key::Back:            return "BACK";
	case Key::Menu:            return "MENU";
	case Key::TouchPad:        return "touch pad";
	case Key::Unknown:         return "unknown";
	}
	return "?";
}

const char *action_name(Action a)
{
	switch (a) {
	case Action::Press:   return "press";
	case Action::Release: return "release";
	case Action::Turn:    return "turn";
	case Action::Touch:   return "touch";
	case Action::Power:   return "power";
	}
	return "?";
}

std::string describe(const Event &e)
{
	char buf[64];

	switch (e.action) {
	case Action::Turn:
		std::snprintf(buf, sizeof(buf), "%s turn %+d", key_name(e.key), (int)e.detents);
		break;
	case Action::Power:
		std::snprintf(buf, sizeof(buf), "power %02x", e.power);
		break;
	case Action::Touch: {
		int len = std::snprintf(buf, sizeof(buf), "touch pad");
		for (int i = 0; i < e.touch_len && len < (int)sizeof(buf) - 4; i++)
			len += std::snprintf(buf + len, sizeof(buf) - len, " %02x", e.touch[i]);
		break;
	}
	default:
		if (e.key == Key::Unknown)
			std::snprintf(buf, sizeof(buf), "unknown b%d 0x%02x %s", e.byte, e.mask,
			              action_name(e.action));
		else
			std::snprintf(buf, sizeof(buf), "%s %s", key_name(e.key), action_name(e.action));
		break;
	}
	return buf;
}

} // namespace buttons

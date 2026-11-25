// buttons.h - the unit's knobs and buttons as events, decoded from
// /dev/ipc/ch8 (the protocol: src/common/buttons/README.md).
//
// A ch8 message is the state of every control at once: knob bytes carry the
// detents turned since the last message, buttons are bits that stay set while
// held. Decoder keeps the previous button state and turns each message into
// events: Press / Release per button bit, Turn per knob byte, Touch for the
// touch pad's messages, Power when byte 15 (the motorised screen) changes.
//
// Pure (no car APIs), tested on the host (tests/test_buttons.cpp). hub.h
// hands the events to any number of observers.
#ifndef BUTTONS_BUTTONS_H
#define BUTTONS_BUTTONS_H

#include "ch8.h"
#include "types.h"

#include <string>
#include <vector>

namespace buttons {

// A physical control. A knob turns and can be pressed.
enum class Key : u8 {
	None,           // Power events
	MainKnob,       // turn: + clockwise (right), - anticlockwise; press
	VolumeKnob,     // turn: + up, - down; press (mute and unmute alike)
	Radio,
	Media,
	Tel,
	Nav,
	Car,
	Next,           // >>|
	Prev,           // |<<
	SoftTopLeft,
	SoftBottomLeft,
	SoftTopRight,
	SoftBottomRight,
	Back,
	Menu,
	TouchPad,
	Unknown,        // a control bit no table entry names (byte, mask)
};

enum class Action : u8 {
	Press,
	Release,
	Turn,           // detents (signed)
	Touch,          // the touch pad's raw bytes (not decoded yet)
	Power,          // byte 15 changed: the motorised screen, 02 = out, 03 / 00 = folding away (ignition off)
};

constexpr int kTouchMax = 6;

struct Event {
	Key key = Key::None;
	Action action = Action::Press;
	i32 detents = 0;        // Turn
	u8 byte = 0, mask = 0;  // Press / Release: where the bit is
	u8 power = 0;           // Power: the new byte 15
	u8 touch_len = 0;       // Touch: bytes after byte 26
	u8 touch[kTouchMax] = {};
	u32 ms = 0;             // when it arrived (the caller's clock)
};

// The message layout.
constexpr int kMsgBytes = CH8_MSG_BYTES;         // a plain message; touch pad ones are 29-33
constexpr int kControlBytes = CH8_CONTROL_BYTES; // 0-13: buttons and knobs
constexpr int kKindByte = CH8_KIND_BYTE;         // 01 = controls, 02 = touch pad / extra bytes
constexpr int kPowerByte = CH8_POWER_BYTE;       // 02 while running

class Decoder {
public:
	// Appends the events of buf[0..n) to out. Short messages (< 16 bytes)
	// give nothing.
	void decode(const u8 *buf, int n, u32 ms, std::vector<Event> &out);
	// Forget the held buttons and the power state.
	void reset();

	// Bits held now, per control byte (knob bytes always 0).
	u8 held(int byte) const { return byte >= 0 && byte < kControlBytes ? held_[byte] : 0; }

private:
	u8 held_[kControlBytes] = {};
	u8 power_ = 0;          // 0 = not seen yet
};

// "main knob", "BACK", ...
const char *key_name(Key k);
const char *action_name(Action a);
// "main knob turn +2", "BACK press", "unknown b9 0x10 press", "touch pad ca 84 c6",
// "power 03".
std::string describe(const Event &e);

// A main knob click: the press that starts things in a menu.
inline bool is_knob_press(const Event &e)
{
	return e.key == Key::MainKnob && e.action == Action::Press;
}

// Which key a button bit is: Key::Unknown if no table entry names it.
Key key_of(int byte, int mask);

} // namespace buttons

#endif

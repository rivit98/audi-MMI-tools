// model.h - what audi_stream_receiver shows besides the picture: the link state.
// Written by the USB threads, read by the render loop, as a copy.
#ifndef AUDI_STREAM_MODEL_H
#define AUDI_STREAM_MODEL_H

#include <string>

namespace model {

enum class LinkState {
	NoStack,    // io-usb not reachable (yet); retrying
	Waiting,    // no phone
	Switching,  // AOA start sent, waiting for the phone to come back
	Accessory,  // phone in accessory mode, its app has not said hello
	Connected,  // app said hello (or sent a picture)
};

constexpr unsigned kDetailMax = 119;

struct Model {
	LinkState   state = LinkState::NoStack;
	std::string detail;
	bool        details = false;  // status bar over a landscape picture
	unsigned    version = 0;      // bumped on every change
};

Model get();
unsigned version();

// Logs the transition.
void set_state(LinkState state, const char *detail);
// VIDEO_DETAILS from the phone. Logs a change.
void set_details(bool show);

} // namespace model

#endif

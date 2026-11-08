// model.cpp - shared app state (see model.h).
#include "model.h"

#include <mutex>
#include <utility>

extern "C" {
#include "log.h"
}

namespace model {

namespace {

std::mutex mu;
Model current;

const char *state_name(LinkState state)
{
	switch (state) {
	case LinkState::NoStack:   return "NO_STACK";
	case LinkState::Waiting:   return "WAITING";
	case LinkState::Switching: return "SWITCHING";
	case LinkState::Accessory: return "ACCESSORY";
	case LinkState::Connected: return "CONNECTED";
	}
	return "?";
}

} // namespace

Model get()
{
	std::lock_guard<std::mutex> lock(mu);
	return current;
}

unsigned version()
{
	std::lock_guard<std::mutex> lock(mu);
	return current.version;
}

void set_state(LinkState state, const char *detail)
{
	std::string d(detail);
	if (d.size() > kDetailMax)
		d.resize(kDetailMax);
	std::lock_guard<std::mutex> lock(mu);
	if (current.state != state || current.detail != d) {
		log_msg("state %s -> %s: %s", state_name(current.state), state_name(state), d.c_str());
		current.state = state;
		current.detail = std::move(d);
		current.version++;
	}
}

void set_details(bool show)
{
	std::lock_guard<std::mutex> lock(mu);
	if (current.details != show) {
		log_msg("details over a landscape picture: %s", show ? "on" : "off");
		current.details = show;
		current.version++;
	}
}

} // namespace model

// aoa_host.h - an Android Open Accessory host on the MMI's io-usb stack
// (libusbdi) that carries a plain byte stream: the USB side of android_auto.
//
// The same as audi_stream_receiver's aoa.cpp (which it is derived from, with
// every lesson learnt there; docs/usb-stack.md), minus that app's own
// protocol: the identification strings come from the caller, and what
// arrives on bulk IN goes to the caller as bytes.
//
// A manager thread owns everything:
//   1. usbd_connect() to io-usb with an all-wildcard filter (retried every
//      2 s while io-usb is not there).
//   2. Any new device, 4 s after it appears (the media launcher probes it
//      first): AOA handshake on its control pipe (GET_PROTOCOL 51, the six
//      strings 52, START 53). A device that refuses GET_PROTOCOL is left
//      alone; other failures are retried up to 5 times.
//   3. The phone re-enumerates as 18d1:2d00..2d05. Its accessory interface
//      (ff/ff) gets a bulk IN and OUT pipe; IN is read continuously
//      (Client::rx, on io-usb's thread), OUT sends what send() queued.
//   4. Removal or a transfer error closes the link (Client::closed); a
//      link that broke without a removal is reopened (up to 5 times).
//
// Client callbacks other than rx run on the manager thread.
#ifndef COMMON_AOA_HOST_H
#define COMMON_AOA_HOST_H

#include <cstddef>
#include <cstdint>

namespace aoa_host {

enum class State {
	NoStack,     // io-usb not there (yet)
	Waiting,     // no phone
	Switching,   // AOA START sent, waiting for the accessory
	Open,        // bulk pipes open
};
const char *state_name(State s);

// System log snapshots per run: <sd>/var/usb_slog_1.txt ... _<this>.txt
constexpr int kSlogSnapshots = 6;

struct Config {
	// manufacturer, model, description, version, URI, serial
	const char *strings[6];
	// <sd>/var/usb_slog_<n>.txt on a failed handshake or a removal;
	// nullptr: none
	const char *sd_path = nullptr;
	// Client::tick every this many ms (manager thread)
	unsigned    tick_ms = 100;
};

class Client {
public:
	virtual ~Client() = default;
	// Every state change, with a line for the screen.
	virtual void state(State, const char * /*detail*/) {}
	// The link is open: send() works from now on.
	virtual void opened() {}
	// Bytes from bulk IN. On io-usb's callback thread: copy, don't block.
	virtual void rx(const uint8_t *p, size_t n) = 0;
	// The link is gone (removal, error, stop).
	virtual void closed(const char * /*why*/) {}
	virtual void tick() {}
};

// false if the manager thread could not start.
bool start(const Config &cfg, Client &client);
void stop();    // tear down and join; bounded by quit.c's hard alarm

// Queue bytes for bulk OUT (any thread, copied). false: no link, or more
// than kOutQueueMax bytes waiting (the link is stuck).
constexpr size_t kOutQueueMax = 1u << 20;
bool send(const uint8_t *p, size_t n);

// Bytes queued for bulk OUT and not yet sent.
size_t out_pending();

} // namespace aoa_host

#endif

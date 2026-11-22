// hub.h - hand the decoded button events (buttons.h) to any number of
// observers in one process.
//
// The process reads /dev/ipc/ch8 once: quit.c already does, on its key
// thread, for every app, and must keep doing so (a reader that stops reading
// stalls the channel for every other reader, the HMI included; README.md).
// Plug the hub into it:
//
//     buttons::Hub &hub = *new buttons::Hub;   // never freed, see below
//     quit_init_ex(sd_path, watchdog_s, buttons::Hub::quit_cb, &hub);
//
// The hub must outlive quit.c's key thread, which runs until the process
// exits (blocked in read(), not joined): allocate it once and never free it.
//
// Then either subscribe a callback (it runs on the key thread: keep it short,
// never block) or, for a main loop, take the events from an EventQueue:
//
//     buttons::EventQueue q(hub);
//     buttons::Event e;
//     while (q.wait(e, 100)) { ... }
//
// Callbacks run with the hub's lock held: they must not subscribe or
// unsubscribe (that deadlocks). unsubscribe() returns only once no callback
// of that id is running, so the callback's captures may be freed after it.
//
// The hub logs (log.h) every event with the subscribers it went to
// ("buttons: BACK press -> queue, hello5"), every subscribe / unsubscribe
// and every event an EventQueue drops. set_logging(false) silences it.
#ifndef BUTTONS_HUB_H
#define BUTTONS_HUB_H

#include "buttons.h"
#include "mono_cond.h"
#include "types.h"

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace buttons {

class Hub {
public:
	using Handler = std::function<void(const Event &)>;

	// Returns the id for unsubscribe (> 0). name: for the log, a string
	// that outlives the subscription (a literal); NULL logs "#<id>".
	int subscribe(Handler h, const char *name = nullptr);
	void unsubscribe(int id);

	// One raw ch8 message, from the reader thread. ms: the caller's clock.
	void feed(const u8 *buf, int n, u32 ms);

	// quit_key_cb for quit_init_ex(); arg is the Hub. Stamps with log_ms().
	static void quit_cb(const u8 *buf, int n, void *arg);

	// Log events and subscriptions (on by default).
	void set_logging(bool on);

	u32 messages() const;
	// Bits held now in a control byte (Decoder::held).
	u8 held(int byte) const;

private:
	struct Sub {
		int id;
		Handler h;
		const char *name;
	};
	void log_event(const Event &e) const;
	static const char *name_of(const Sub &s, char *buf, usize size);

	mutable std::mutex mu_;
	Decoder dec_;
	std::vector<Sub> subs_;
	std::vector<Event> scratch_;
	int next_id_ = 1;
	u32 messages_ = 0;
	bool logging_ = true;
};

// The events of one hub, queued for a main loop. Subscribes in the
// constructor, unsubscribes in the destructor. When full, the oldest event
// is dropped (dropped() counts them).
class EventQueue {
public:
	// name: the subscriber's name in the hub's log (a literal).
	explicit EventQueue(Hub &hub, usize capacity = 64, const char *name = "queue");
	~EventQueue();
	EventQueue(const EventQueue &) = delete;
	EventQueue &operator=(const EventQueue &) = delete;

	// The next event, waiting up to timeout_ms for one. false: none came.
	bool wait(Event &e, unsigned timeout_ms);
	// The next event if there is one, without waiting.
	bool poll(Event &e);
	// Forget everything queued (presses made while the app was busy).
	void clear();
	usize size() const;
	u32 dropped() const;

private:
	void push(const Event &e);

	Hub &hub_;
	usize cap_;
	const char *name_;
	int id_;
	mutable std::mutex mu_;
	MonoCond cond_;
	std::deque<Event> q_;
	u32 dropped_ = 0;
};

} // namespace buttons

#endif

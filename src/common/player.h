// player.h - one io-media trackplayer graph through our own client
// (common/iom.c, the calls testapp-file makes): an audio output, one input
// URL, play, and its events turned into numbers.
#ifndef COMMON_PLAYER_H
#define COMMON_PLAYER_H

#include "types.h"
#include <functional>
#include <string>
#include <vector>

struct PlayStats {
	unsigned playing_ms = 0;       // first STATUS state 4 (playing), after play()
	unsigned first_time_ms = 0;    // first TIME event
	unsigned time_events = 0;
	i64 max_pos_ms = -1;
	unsigned error_events = 0;
	std::string first_error;
	bool input_eof = false, finished = false;
};

// One io-media event, for the dump after an error (Player::recent_events).
struct PlayEvent {
	unsigned at_ms = 0;            // log_ms() when read
	u32 type = 0;
	i64 a = 0, b = 0;          // TIME: position, total; STATUS: state, speed;
	                               // ERROR: iom_errno, value
};

class Player {
public:
	static constexpr unsigned kEventHistory = 16;

	~Player() { close(); }

	// Create graph <name>, attach the first output that works and the input.
	// "" or what failed (with io-media's iom_errno).
	std::string open(const std::string &name, const std::string &url);
	std::string play();
	// Read and log the events waiting. on_time(ms since play, position ms)
	// for every TIME event.
	void poll(const std::function<void(unsigned, i64)> &on_time);
	void close();

	const PlayStats &stats() const { return st_; }
	// The last kEventHistory events, oldest first.
	std::vector<PlayEvent> recent_events() const;
	// "TIME 12.3 s of 3600.0 s", "STATUS state 4 speed 1000", ...
	static std::string describe(const PlayEvent &e);
	const std::string &name() const { return name_; }
	const char *output() const { return output_; }

private:
	struct iom_state;
	std::string fail(const char *what, int rc);
	void event(u32 type, const unsigned char *p, u32 len,
	           const std::function<void(unsigned, i64)> &on_time);

	void remember(u32 type, i64 a, i64 b);

	PlayEvent events_[kEventHistory];
	unsigned events_n_ = 0;
	iom_state *g_ = nullptr;
	std::string name_;
	const char *output_ = nullptr;
	unsigned t_play_ = 0;
	bool blind_ = false;
	PlayStats st_;
};

#endif

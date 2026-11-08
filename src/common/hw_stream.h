// hw_stream.h - a live H.264 stream on the screen through the unit's own
// io-media and DM642 decoder, the path that plays on the car
// (docs/phone-mirroring.md 7.10):
//
//   push(frame) -> live::Stream (CS-MP4 in memory, gap fill)
//               -> StreamServer (resource manager at opts.path, blocking reads)
//               -> io-media trackplayer graph (Player) -> DSP
//               -> VCAP 0 input 1 -> video displayable 41 (vout)
//
// The owner calls poll() every ~20 ms from one thread: once the first IDR
// is in, it creates the graph and plays it; video_after_ms later it puts
// the picture on screen; it reads io-media's events throughout. push() may
// come from any thread. The CPU-drawn screen (displayable 40) should be
// closed while the video is on; vout replaces what the layer manager shows.
//
// Used by audi_stream_receiver (the phone's frames) and hello9 (a clip on
// the card, fed like a phone would), so hello9's car runs test the
// receiver's code.
#ifndef COMMON_HW_STREAM_H
#define COMMON_HW_STREAM_H

#include "live_stream.h"
#include "player.h"
#include "stream_server.h"
#include "vout.h"

#include <cstdint>
#include <functional>
#include <string>

struct HwOptions {
	live::Config cfg;
	std::string path = "/dev/hw_stream/live.mp4";   // must end in .mp4
	std::string graph = "hw_stream";                 // io-media graph name
	unsigned video_after_ms = 1000;                  // play -> picture on
	bool show_video = true;
	// the picture is on already (a restart: the last stream was stopped
	// with keep_video), so /dev/videoctrl isn't touched again, other than
	// for a new vcap_mode
	bool video_on_already = false;
	vout::Mode vcap_mode = vout::Mode::Keep;
};

class HwStream {
public:
	~HwStream() { stop(); }

	// Configure the stream and start serving it (no graph yet). "" or what
	// failed.
	std::string start(const HwOptions &o);
	// One frame (an access unit, Annex-B). Any thread.
	live::Push push(const uint8_t *annexb, size_t n, std::string *why = nullptr);
	// Owner thread, every ~20 ms. on_time(ms since play, position ms) for
	// every io-media TIME event. Returns "" or what failed (the graph would
	// not play; the caller stops).
	std::string poll(const std::function<void(unsigned, int64_t)> &on_time = {});
	// Picture off, graph gone, stream ended, server gone. Safe to repeat.
	// keep_video: the displayable and VCAP stay as they are, for the next
	// stream (HwOptions::video_on_already) or vout::hide(false).
	//
	// /dev/videoctrl is shared with the HMI's rear camera, and nothing tells
	// an app whether the camera is on (docs/rear-camera.md §9, §11): setting
	// VP1 while it is hides it, and so does disabling VP1. So the picture is
	// switched only when the user starts / stops a stream, never on a
	// restart.
	void stop(bool keep_video = false);

	bool active() const { return server_.running(); }
	bool playing() const { return playing_; }
	bool video_on() const { return video_on_; }
	unsigned play_ms() const { return t_play_; }      // log_ms() at play, 0 = not yet
	unsigned video_ms() const { return t_video_; }    // log_ms() at picture on

	// How long after frame (position pos_ms) was pushed io-media reports
	// playing it, in ms; INT64_MIN if that frame is unknown.
	int64_t lag_ms(int64_t pos_ms) const;

	live::Stream &stream() { return stream_; }
	StreamServer &server() { return server_; }
	Player &player() { return player_; }
	const HwOptions &options() const { return opts_; }

private:
	HwOptions opts_;
	live::Stream stream_;
	StreamServer server_;
	Player player_;
	bool playing_ = false, video_on_ = false, failed_ = false;
	unsigned t_play_ = 0, t_video_ = 0;
};

#endif

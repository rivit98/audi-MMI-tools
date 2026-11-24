// audio_rx.h - the phone's audio stream (AUDIO_CONFIG / AUDIO_DATA /
// AUDIO_STOP, proto.h) checked and counted, not played. Pure:
// tests/test_audio_rx.cpp (BlueZ's encoder makes the frames).
//
// Every frame is checked (header as configured, CRC, whole frame), the
// frame numbers for gaps, and the arrival against the phone's capture
// clock. The caller logs one line per window (window()) and one when the
// stream ends (end()), and may save the first good frames (capture) to a
// file a desk tool can play (ffmpeg reads raw SBC).
#ifndef AUDI_STREAM_AUDIO_RX_H
#define AUDI_STREAM_AUDIO_RX_H

#include "proto.h"
#include "types.h"

#include <string>
#include <vector>

namespace audio {

constexpr unsigned kWindowMs = 10000;

struct Counts {
	unsigned msgs = 0;          // AUDIO_DATA messages
	unsigned frames = 0;        // good frames
	unsigned bad = 0;           // frames refused: header, CRC, cut short, not as configured
	unsigned missing = 0;       // frame numbers skipped (seq gaps)
	unsigned back = 0;          // messages whose seq went backwards
	unsigned silent = 0;        // good frames with every scale factor 0
	unsigned short_msgs = 0;    // AUDIO_DATA shorter than its header
	u64 bytes = 0;              // good frames' bytes
	unsigned gap_max_ms = 0;    // longest time between two messages
	i64 lag_min = 0, lag_max = 0, lag_last = 0; // arrival - capture, vs the first message (ms)
	bool lag_seen = false;
};

class Rx {
public:
	// capture_bytes: how many bytes of good frames to keep (0: none).
	explicit Rx(usize capture_bytes) : cap_(capture_bytes) {}

	// AUDIO_CONFIG: "" if taken, else why not (the stream is then off until
	// the next good one). A stream already running ends first (its end
	// line in *ended, else empty).
	std::string config(const u8 *p, unsigned n, unsigned now_ms, std::string *ended);
	// AUDIO_DATA. Ignored without a config.
	void data(const u8 *p, unsigned n, unsigned now_ms);
	// The window's line once kWindowMs have passed since it started (or
	// now, with force and anything in it); "" otherwise. Starts the next.
	std::string window(unsigned now_ms, bool force = false);
	// The stream ended (AUDIO_STOP, a new config, the link down): the last
	// window and the totals, "" if no stream ran.
	std::string end(const char *why, unsigned now_ms);

	bool active() const { return on_; }
	const Counts &totals() const { return total_; }
	const proto::AudioConfig &cfg() const { return cfg_; }

	// The first capture_bytes of good frames of the stream (or all it had
	// when it ended), once: empty until then and after it was taken.
	std::vector<u8> take_capture();

private:
	void count_frame(const u8 *f, bool good);
	std::string line(const char *what, const Counts &c, unsigned ms) const;

	usize cap_;
	bool on_ = false;
	proto::AudioConfig cfg_;
	u32 samples_ = 0;           // per frame per channel
	u32 next_seq_ = 0;
	unsigned t0_ms_ = 0, win_ms_ = 0, last_msg_ms_ = 0;
	bool any_msg_ = false;
	u32 phone0_ = 0;            // the first message's phone_ms
	unsigned mmi0_ = 0;         // and when it came
	Counts win_, total_;
	std::vector<u8> capture_;
	bool capture_done_ = false, capture_taken_ = false;
};

} // namespace audio

#endif

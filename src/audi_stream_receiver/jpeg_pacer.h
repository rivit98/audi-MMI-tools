// jpeg_pacer.h - how often the main loop may take the next JPEG picture.
// Pure (no locking, no clock: the times are passed in; main loop only),
// tests/test_pacer.cpp.
//
// A JPEG costs the main thread ~66 ms to decode and ~23 ms to render, plus
// 25 ms (a still screen) to ~160 ms (all of it changed) to copy and flip.
// Unpaced, a phone that sends as fast as it can kept the main thread
// 91-100 % busy for minutes, and the unit's own HMI and navigation starved.
// So two limits, whichever is later:
//
//   rate   at most kMaxJpegFps pictures a second: the next is taken no
//          sooner than kMinIntervalMs after the last was taken
//   duty   a picture that took T ms of main-thread time (decode, render,
//          copy, flip) is followed by at least T ms of rest, so the main
//          thread stays at ~50 % or less however big the pictures are
//
// The phone sends the next picture only when fewer than two are in flight
// (AccessoryLink.kt), so video.cpp holds the shown picture's ack until
// ready(): the phone then waits instead of sending pictures that would only
// be thrown away. T is counted up to kMaxBusyMs, which keeps a held ack far
// from the phone's 3 s ack timeout.
#ifndef AUDI_STREAM_JPEG_PACER_H
#define AUDI_STREAM_JPEG_PACER_H

class JpegPacer {
public:
	static constexpr unsigned kMaxJpegFps = 5;
	static constexpr unsigned kMinIntervalMs = 1000 / kMaxJpegFps;
	static constexpr unsigned kMaxBusyMs = 1000;

	// A new stream: the first picture goes at once.
	void reset() { have_ = false; }

	// The main loop took a picture to decode.
	void taken(unsigned now_ms)
	{
		have_ = true;
		taken_ms_ = now_ms;
		next_ms_ = now_ms + kMinIntervalMs;
	}

	// Its main-thread work is done (shown, or found bad): busy_ms of it.
	void done(unsigned now_ms, unsigned busy_ms)
	{
		if (!have_)
			return;
		if (busy_ms > kMaxBusyMs)
			busy_ms = kMaxBusyMs;
		busy_ms_ = busy_ms;
		unsigned rest = now_ms + busy_ms;
		if (after(rest, next_ms_))
			next_ms_ = rest;
	}

	// May the next picture be taken (and the last one's ack go out)?
	// now_ms may wrap.
	bool ready(unsigned now_ms) const { return !have_ || !after(next_ms_, now_ms); }
	// How long until ready(), 0 if it is.
	unsigned wait_ms(unsigned now_ms) const { return ready(now_ms) ? 0 : next_ms_ - now_ms; }
	// When the next may be taken (valid while a picture was taken).
	unsigned next_ms() const { return next_ms_; }
	bool active() const { return have_; }
	unsigned last_busy_ms() const { return busy_ms_; }

private:
	// a later than b, across the wrap
	static bool after(unsigned a, unsigned b) { return (int)(a - b) > 0; }

	bool have_ = false;
	unsigned taken_ms_ = 0, next_ms_ = 0, busy_ms_ = 0;
};

#endif

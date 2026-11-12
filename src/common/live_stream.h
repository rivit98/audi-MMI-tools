// live_stream.h - a live H.264 stream as a CS-MP4 (csmp4.h) in memory: the
// header, then frames as the sender pushes them. stream_server.h serves it
// to io-media; tests/test_live.cpp runs it on the build host (pure C++, the
// time is passed in).
//
// What push() does with a frame (one access unit, Annex-B):
//   - nothing before the first IDR: the decoder needs one to start
//   - AUDs dropped, the rest stored with 4-byte lengths, padded to S
//   - a frame too big for S is dropped; if it was a reference frame the
//     frames after it are dropped too, up to the next IDR (they refer to
//     it). The caller asks the sender for an IDR (sync_wanted()).
//   - in-band SPS/PPS different from the configured ones: refused, the
//     caller has to restart with the new config (a new header)
//
// Gap fill (fill()): io-media keeps ~9 frames queued and shows them only
// when more data comes, and skips them then (docs/phone-mirroring.md
// 7.10). So when the stream is two or more frames behind its frame clock
// (frame k due k/fps after the first) and nothing has been pushed for a
// frame period, fill() appends skip frames (h264.h): non-reference P
// frames that repeat the picture, up to one frame short of the clock. A
// sender on time is never filled, a slow one (15 fps) is made up to the
// stream's rate, and a pause is filled at the stream's rate. Skips never
// put the stream ahead of the clock, so they add no latency.
//
// Clock cap (Config::shed_ahead_ms, 0 = off): io-media plays exactly fps,
// so a sender faster than that (26 fps against 25), or a burst after a
// stall, puts the stream ahead of its frame clock and every frame after it
// plays that much later - the lag only grows. When the stream gets more
// than shed_ahead_ms ahead, the frames pushed are dropped (Push::Shed)
// until it is back on the clock, then an IDR is wanted (sync_wanted()) and
// the stream goes on from it. On screen: the picture plays the backlog,
// then jumps to the present.
//
// Thread-safe: push() from the receiving thread, fill() and read() from the
// server's.
#ifndef COMMON_LIVE_STREAM_H
#define COMMON_LIVE_STREAM_H

#include "csmp4.h"
#include "h264.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace live {

struct Config {
	std::vector<uint8_t> sps, pps;   // NAL units, header byte first, no start code
	unsigned fps = 25;
	uint32_t sample_size = 32768;    // S, every frame padded to it
	uint32_t declared = 0;           // frames in the header; 0 = as many as fit in 4 GB
	bool fill = true;
	unsigned fill_after_ms = 0;      // nothing pushed for this long (0: one frame
	                                 // period) and 2+ frames behind: fill
	unsigned ring_frames = 128;      // frames kept for io-media to read (S each)
	unsigned shed_ahead_ms = 0;      // more ahead of the frame clock: drop up to
	                                 // it, then wait for an IDR (0: never)
};

enum class Push {
	Ok,
	WaitIdr,          // dropped: no IDR yet (start, or after a dropped frame)
	TooBig,           // dropped: bigger than S
	Bad,              // dropped: no slice, unparsable, another PPS
	ConfigChanged,    // dropped: in-band SPS/PPS differ: restart with them
	Full,             // dropped: the declared frames are all used: restart
	Shed,             // dropped: the stream is too far ahead of its clock
	NotConfigured,
};
const char *push_name(Push p);

// Will the unit's io-media and DM642 take this stream? "" or why not:
// H.264 Baseline (profile 66), level 3.0 at most, at most 720x576
// (io-media refuses wider / taller, FUN_080d1760), frames only, 1..30 fps
// (more than 25 fps above 480 lines failed in hello6).
std::string check_decodable(const h264::Sps &sps, unsigned fps);

struct Stats {
	uint32_t frames = 0;        // in the stream (real + skips)
	uint32_t real = 0, idr = 0, skips = 0;
	uint32_t too_big = 0, wait_idr = 0, bad = 0, config_changed = 0;
	uint32_t evicted_reads = 0; // reads of frames no longer kept (got filler)
	uint32_t max_frame = 0;     // largest frame pushed (bytes, before padding)
	uint32_t gaps = 0;          // times fill() started filling
	uint32_t shed = 0;          // frames dropped ahead of the clock
	uint32_t sheds = 0;         // times the stream got too far ahead
	int64_t ahead_ms_max = 0;   // furthest ahead of the clock a frame was pushed
};

class Stream {
public:
	// Build the header; drops whatever was there. "" or what is wrong.
	std::string configure(const Config &c);
	const Config &config() const { return cfg_; }
	const h264::Sps &sps() const { return sps_; }
	bool skips_possible() const { return skip_ok_; }

	Push push(const uint8_t *annexb, size_t n, uint64_t now_ms, std::string *why = nullptr);
	// Appends skip frames if a gap calls for it; returns how many.
	unsigned fill(uint64_t now_ms);
	// End of stream: reads past the last frame get EOF from now on.
	void finish();
	bool finished() const;
	// An IDR is wanted from the sender (a frame was dropped); cleared by
	// the call.
	bool sync_wanted();

	// ---- serving ----
	uint64_t data_off() const { return data_off_; }
	uint32_t sample_size() const { return cfg_.sample_size; }
	uint32_t declared() const { return declared_; }
	uint64_t total() const { return data_off_ + (uint64_t)declared_ * cfg_.sample_size; }
	// Bytes [0, available()) exist now.
	uint64_t available() const;
	uint64_t frames() const;
	// Copy [off, off + n) into dst; n is clamped at total(). Bytes past
	// available() (a "far" read) come as filler frames. Returns the count.
	size_t read(uint64_t off, uint8_t *dst, size_t n);
	// When frame k was pushed (real or skip), or -1 if unknown / no longer kept.
	int64_t push_ms(uint64_t k) const;
	bool started() const;        // the first IDR is in
	uint64_t start_ms() const;   // when it came
	Stats stats() const;

private:
	void store(const std::vector<uint8_t> &sample, uint64_t now_ms);

	mutable std::mutex mu_;
	Config cfg_;
	h264::Sps sps_;
	h264::Pps pps_;
	bool skip_ok_ = false;
	std::vector<uint8_t> header_;
	uint64_t data_off_ = 0;
	uint32_t declared_ = 0;

	std::vector<uint8_t> ring_;          // ring_frames * S
	std::vector<int64_t> ring_ms_;
	uint64_t frames_ = 0;
	bool started_ = false, need_idr_ = false, finished_ = false, sync_ = false;
	uint64_t t0_ms_ = 0, last_push_ms_ = 0;
	bool filling_ = false, shedding_ = false;
	unsigned prev_ref_frame_num_ = 0, last_poc_lsb_ = 0;
	std::vector<uint8_t> sample_, filler_;
	Stats st_;
};

} // namespace live

#endif

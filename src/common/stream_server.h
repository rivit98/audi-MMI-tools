// stream_server.h - a QNX resource manager that serves a live H.264 stream
// (live_stream.h, a CS-MP4 in memory) at a path io-media can open, e.g.
// /dev/audi_stream/live.mp4. Plays on the car (hello9 "live",
// docs/phone-mirroring.md 7.10).
//
// The header is there at once; frame k once it has been pushed. A read that
// reaches past the frames pushed so far gets no reply until they are: it
// blocks, as reading a live stream would. io-media reads it in order, 64 KB
// at a time, and waits as long as it takes (5 s seen).
//
// One thread runs the resource manager: io_read either answers at once or
// queues the request, and every 10 ms (dispatch timeout) the stream's gap
// fill runs (live::Stream::fill) and the queue is served as far as it can
// be. start() and stop() are called from the owner's thread.
//
// The path must end in .mp4: io-media picks its demuxer by extension.
#ifndef COMMON_STREAM_SERVER_H
#define COMMON_STREAM_SERVER_H

#include "live_stream.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct StreamStats {
	unsigned opens = 0;
	unsigned reads = 0;           // read requests
	unsigned preads = 0;          // of them, with an offset (pread)
	unsigned waited = 0;          // reads that had to wait for frames
	unsigned eof = 0;             // reads answered with 0 bytes
	unsigned unblocked = 0;       // waiting reads the client gave up on
	unsigned far = 0;             // reads > 10 s past the live edge, answered at once
	uint64_t bytes = 0;
	unsigned min_read = 0, max_read = 0;
	unsigned wait_ms_max = 0;
	unsigned wait_win_max = 0;    // the same since the last stats_window()
	uint64_t wait_ms_sum = 0;
	int64_t lead_frames_max = INT64_MIN;  // how far past the newest frame a read asked
	int64_t lead_frames_last = 0;
	// the same since the last stats_window() (INT64_MAX / INT64_MIN: no read)
	int64_t lead_win_min = INT64_MAX, lead_win_max = INT64_MIN;
	uint64_t served_end = 0;      // highest byte offset answered
	unsigned waiting_now = 0;
};

// One read request, for the dump after an io-media error (the last
// StreamServer::kReadHistory of them).
struct ReadRecord {
	unsigned seq = 0;
	unsigned at_ms = 0;           // log_ms() when it came
	uint64_t off = 0;
	unsigned nbytes = 0;          // asked
	int64_t first = -1, last = -1;  // frames it covers (-1: header only)
	int64_t lead = 0;             // frames past the newest pushed, when it came
	bool answered = false;        // false: still waiting
	unsigned served = 0;          // bytes answered
	unsigned waited_ms = 0;
	bool far = false;             // answered at once with filler (kFarS)
	unsigned evicted = 0;         // frames in it no longer kept: filler
	unsigned ahead = 0;           // frames in it not pushed yet: filler
};

class StreamServer {
public:
	StreamServer() = default;
	~StreamServer();
	StreamServer(const StreamServer &) = delete;
	StreamServer &operator=(const StreamServer &) = delete;

	// Serve stream (configured, outliving the server) at path. "" or what
	// failed.
	std::string start(const char *path, live::Stream *stream);
	// End of stream (stream->finish()), waiting reads get what is there,
	// then the path goes away and the thread stops.
	void stop();
	bool running() const { return impl_ != nullptr; }

	static constexpr unsigned kReadHistory = 32;

	// Live while serving; after stop(), what it ended with, until the next
	// start().
	StreamStats stats();
	// The same, and a new window starts for lead_win_min / max, wait_win_max.
	StreamStats stats_window();
	// The last kReadHistory reads, oldest first (waiting ones included).
	std::vector<ReadRecord> recent_reads();
	// Notes from the server thread (the first reads in detail, unblocks,
	// errors) since the last call. The server never writes the log itself:
	// log.c fsyncs every line, which would hold up its replies.
	std::vector<std::string> take_notes();

	// called by the resource manager callbacks (stream_server.cpp)
	struct Impl;

private:
	Impl *impl_ = nullptr;
	StreamStats last_;
	std::thread thread_;
};

#endif

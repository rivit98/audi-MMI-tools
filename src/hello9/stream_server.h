// stream_server.h - hello9's live stream: a QNX resource manager that serves
// a constant-sample-size MP4 (stream_plan.h) at a path io-media can open,
// releasing its frames in real time the way a phone's stream would arrive.
//
// The header is always there; frame k only from schedule.release_ms(k) after
// go(). A read that reaches past the frames released so far gets no reply
// until they are: it blocks, as reading a live stream would. The frames come
// from a CS-MP4 file on the card, looped when the header declares more than
// the file holds.
//
// One thread runs the resource manager: io_read either answers at once or
// queues the request, and every 10 ms (dispatch timeout) the queue is
// served as far as the schedule allows. go(), finish() and stop() are
// called from the main thread.
#ifndef HELLO9_STREAM_SERVER_H
#define HELLO9_STREAM_SERVER_H

#include "stream_plan.h"

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
	uint64_t wait_ms_sum = 0;
	int64_t lead_ms_max = INT64_MIN;  // how far ahead of release a read asked
	int64_t lead_ms_last = 0;
	uint64_t served_end = 0;      // highest byte offset answered
	unsigned waiting_now = 0;
};

class StreamServer {
public:
	StreamServer() = default;
	~StreamServer();
	StreamServer(const StreamServer &) = delete;
	StreamServer &operator=(const StreamServer &) = delete;

	// Serve src_file (a CS-MP4, header already parsed into m) at path,
	// frames per schedule. Returns "" or what failed.
	std::string start(const char *path, const char *src_file, const plan::CsMp4 &m,
	                  const plan::Schedule &schedule);
	// The stream starts now: frame 0 is released, the rest per schedule.
	void go();
	// End of stream: nothing more is released; waiting reads get what is
	// there, then 0 bytes (EOF).
	void finish();
	// Detach the path and stop the thread (finish() first).
	void stop();

	StreamStats stats();
	uint64_t elapsed_ms() const;            // since go(); 0 before
	uint64_t released() const;              // frames released by now
	// Notes from the server thread (the first reads in detail, unblocks,
	// errors) since the last call. The server never writes the log itself:
	// log.c fsyncs every line, which would hold up its replies.
	std::vector<std::string> take_notes();

	// called by the resource manager callbacks (stream_server.cpp)
	struct Impl;

private:
	Impl *impl_ = nullptr;
	std::thread thread_;
};

#endif

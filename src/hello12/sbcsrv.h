// sbcsrv.h - a QNX resource manager that serves an SBC clip at a path as a
// live stream: frames come out in real time (sbc::released), from the
// first read on, a short prebuffer early. A read past what is out gets no
// reply until it is (blocking, as a live source). Built for io-media's
// "sbc:<path>" input (docs/bt-audio-path.md §4-5), whose reader is a plain
// blocking read().
//
// Everything a client does is counted, and the first requests of each
// kind are noted for the log: opens (with the pid), reads (size, offset,
// wait), devctls (the command code, answered by iofunc_devctl_default),
// unblocks, closes. That shows what io-media's SBC reader asks of a
// stream, the facts a stand-in for /dev/wms/player1 needs.
//
// One thread runs the resource manager with a 5 ms dispatch timeout; each
// pass releases frames and answers the reads that can be answered.
#ifndef HELLO12_SBCSRV_H
#define HELLO12_SBCSRV_H

#include "types.h"

#include <string>
#include <vector>

struct SbcStats {
	unsigned opens = 0, closes = 0, reads = 0, preads = 0, devctls = 0, unblocks = 0;
	unsigned waited = 0;          // reads held for frames
	unsigned short_reads = 0;     // answered with less than asked (not at the end)
	unsigned eof = 0;             // answered with 0 bytes
	unsigned min_read = 0, max_read = 0, wait_ms_max = 0;
	u64 bytes = 0;
	unsigned first_read_ms = 0;   // log_ms() of the first read (0: none)
	unsigned frames_out = 0, frames = 0;
};

class SbcServer {
public:
	SbcServer() = default;
	~SbcServer();
	SbcServer(const SbcServer &) = delete;
	SbcServer &operator=(const SbcServer &) = delete;

	// Serve data (whole SBC frames at offs, kept by the caller) at path.
	// partial: answer reads with what is out (short reads), else hold each
	// read until its whole range is out. "" or what failed.
	std::string start(const char *path, const std::vector<u8> *data, const std::vector<u32> *offs,
	                  u32 samples, u32 rate, unsigned prebuffer_frames, bool partial);
	// Detach the path; waiting reads get EOF first. Safe to call twice.
	void stop();

	SbcStats stats();
	// Notes since the last call (opens, first reads, devctl codes, ...).
	std::vector<std::string> take_notes();

	struct Impl;

private:
	Impl *impl_ = nullptr;
};

#endif

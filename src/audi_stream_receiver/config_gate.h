// config_gate.h - which H264_CONFIGs restart the stream, and how often a
// frame with no stream to go to asks the phone for one. Pure (no locking: the
// owner holds its own), tests/test_config_gate.cpp.
//
// A restart takes ~300 ms, and frames keep coming. If each of them asked for
// the config, and each copy of the config restarted the stream, the stream
// would never get to play. So one config means one restart, whether it is
// running, being set up or still waiting for that.
#ifndef AUDI_STREAM_RECEIVER_CONFIG_GATE_H
#define AUDI_STREAM_RECEIVER_CONFIG_GATE_H

#include <cstdint>
#include <vector>

class ConfigGate {
public:
	static constexpr unsigned kAskEveryMs = 500;

	// A config from the phone. true: new, the stream must (re)start from
	// it. false: a copy of the newest one.
	bool offer(const std::vector<uint8_t> &payload)
	{
		asked_ = false;
		if (payload == current_)
			return false;
		current_ = payload;
		return true;
	}

	// No stream any more and none coming (the phone stopped, it failed).
	void forget()
	{
		current_.clear();
		asked_ = false;
	}

	// A config was taken: a stream is running or on its way.
	bool taken() const { return !current_.empty(); }
	const std::vector<uint8_t> &current() const { return current_; }

	// The phone should send its config (again): true at most once per
	// kAskEveryMs. now_ms may wrap.
	bool ask(unsigned now_ms)
	{
		if (asked_ && now_ms - asked_ms_ < kAskEveryMs)
			return false;
		asked_ = true;
		asked_ms_ = now_ms;
		return true;
	}

private:
	std::vector<uint8_t> current_;
	bool asked_ = false;
	unsigned asked_ms_ = 0;
};

#endif

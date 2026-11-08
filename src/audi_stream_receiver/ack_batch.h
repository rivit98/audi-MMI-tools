// ack_batch.h - the pictures done since the last VIDEO_ACKS (proto.h). Pure
// (no locking: the owner holds its own), tests/test_proto.cpp.
//
// Counts, not a queue of ids: however long the frame goes unsent, nothing is
// lost, and what is unsent stays one frame.
#ifndef AUDI_STREAM_ACK_BATCH_H
#define AUDI_STREAM_ACK_BATCH_H

#include "proto.h"

class AckBatch {
public:
	// At 25 fps a batch is 200 ms, as often as the manager's tick sends what
	// there is (aoa.cpp). Well under the phone's 12 H.264 frames in flight
	// (AccessoryLink.kt), so batching never makes it wait.
	static constexpr unsigned kEveryFrames = 5;
	static constexpr unsigned kMaxCount = 0xffff;

	struct Counts {
		unsigned shown = 0, bad = 0, dropped = 0;

		unsigned total() const { return shown + bad + dropped; }

		void encode(uint8_t *payload) const
		{
			proto::put_u16(payload, shown);
			proto::put_u16(payload + 2, bad);
			proto::put_u16(payload + 4, dropped);
		}
	};

	void add(proto::PicStatus status)
	{
		switch (status) {
		case proto::PicStatus::Shown: counts_.shown++; break;
		case proto::PicStatus::Bad: counts_.bad++; break;
		case proto::PicStatus::Dropped: counts_.dropped++; break;
		}
	}

	bool full() const { return counts_.total() >= kEveryFrames; }

	// What fits in one VIDEO_ACKS, which then is no longer counted here.
	Counts take()
	{
		Counts c;
		c.shown = take_one(counts_.shown);
		c.bad = take_one(counts_.bad);
		c.dropped = take_one(counts_.dropped);
		return c;
	}

	// A VIDEO_ACKS that could not be sent: goes out with the next one.
	void put_back(const Counts &c)
	{
		counts_.shown += c.shown;
		counts_.bad += c.bad;
		counts_.dropped += c.dropped;
	}

	void clear() { counts_ = Counts(); }

private:
	static unsigned take_one(unsigned &count)
	{
		unsigned n = count < kMaxCount ? count : kMaxCount;
		count -= n;
		return n;
	}

	Counts counts_;
};

#endif

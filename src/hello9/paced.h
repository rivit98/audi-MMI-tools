// paced.h - a constant-size A/V MP4 (media/csav.py) from the card, served
// to io-media as if it were being made live: the header at once, chunk k
// (nv video frames + na audio frames, slot_ms of play time) only once its
// last frame would exist, (k + 1) * slot_ms after start(). Reads past that
// wait in StreamServer, as with the phone's stream. Pure C++
// (tests/test_live.cpp runs it on the build host; the time is passed in).
#ifndef HELLO9_PACED_H
#define HELLO9_PACED_H

#include "stream_source.h"
#include "types.h"

#include <atomic>
#include <string>
#include <vector>

namespace paced {

// The layout csav.py writes, read back from the file's own tables: the
// first trak's stco (chunk offsets: data_off and the chunk size) and stsc
// (video frames per chunk).
struct Layout {
	u64 data_off = 0;
	u32 chunk = 0;           // bytes
	u32 chunks = 0;
	u32 video_per_chunk = 0;
};
std::string parse_layout(const std::vector<u8> &file, Layout &out);

class PacedFile : public StreamSource {
public:
	// The whole file; fps = the video track's frame rate. "" or what is wrong.
	std::string configure(std::vector<u8> file, unsigned fps);
	const Layout &layout() const { return lay_; }
	unsigned slot_ms() const { return slot_ms_; }
	// The clock starts: chunk k is released at now_ms + (k + 1) * slot_ms.
	void start(u64 now_ms);

	u64 data_off() const override { return lay_.data_off; }
	u32 sample_size() const override { return lay_.chunk; }
	u32 declared() const override { return lay_.chunks; }
	u64 total() const override { return data_.size(); }
	unsigned slots_per_s() const override;
	u64 frames() const override { return released_; }
	u64 available() const override;
	usize read(u64 off, u8 *dst, usize n, ReadInfo *info) override;
	unsigned fill(u64 now_ms) override;
	void finish() override { finished_ = true; }
	bool finished() const override { return finished_; }

private:
	std::vector<u8> data_;
	Layout lay_;
	unsigned slot_ms_ = 0;
	u64 t0_ = 0;
	std::atomic<bool> started_{false}, finished_{false};
	std::atomic<u32> released_{0};   // 32-bit: SH4 has no 64-bit atomics
};

} // namespace paced

#endif

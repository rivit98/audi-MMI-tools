// vasm.h - reassembles one VIDEO_START + VIDEO_DATA... sequence (proto.h)
// into a complete picture. Pure: no threads, no USB; the link owns one and
// feeds it from its frame callback. tests/test_proto.cpp.
//
// One picture is assembled at a time. A VIDEO_START while one is still
// incomplete drops that one (the phone gave up on it); DATA for any other
// id, or with no picture started, is ignored.
#ifndef AUDI_STREAM_VASM_H
#define AUDI_STREAM_VASM_H

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include "proto.h"

// A complete picture.
struct Picture {
	std::unique_ptr<uint8_t[]> data;
	uint32_t size = 0;
	unsigned id = 0, w = 0, h = 0, format = 0;     // format: proto::kFmt*
	unsigned phone_ms = 0;                         // the phone's scale + encode, 0 = not sent
	unsigned rx_start_us = 0, rx_done_us = 0;      // log_us(), set by the link (perf)
};

class Assembler {
public:
	enum class Rx {
		Ok,       // accepted, picture not complete yet
		Done,     // picture complete: take() it
		Ignored,  // DATA for no picture / another id, or a short frame
		Bad,      // picture refused or broken (id in Result)
	};
	struct Result {
		Rx  rx;
		int id = 0;            // Bad: which picture
		int cut_short = -1;    // start(): id of an incomplete picture this one replaced
	};

	explicit Assembler(uint32_t max_bytes) : max_bytes_(max_bytes) {}

	// VIDEO_START payload. Bad: size 0 / over max_bytes, unknown format,
	// zero width or height, out of memory.
	Result start(const uint8_t *p, unsigned n)
	{
		Result r;
		r.rx = Rx::Bad;
		r.cut_short = active_ ? (int)cur_.id : -1;
		reset();
		if (n < proto::kVideoStartLen) {
			r.id = n >= 2 ? proto::get_u16(p) : 0;
			return r;
		}
		Picture pic;
		pic.id = proto::get_u16(p);
		pic.w = proto::get_u16(p + 2);
		pic.h = proto::get_u16(p + 4);
		pic.format = p[6];
		pic.size = proto::get_u32(p + 7);
		pic.phone_ms = n >= proto::kVideoStartLen + 2 ? proto::get_u16(p + 11) : 0;
		r.id = (int)pic.id;
		if (pic.size == 0 || pic.size > max_bytes_ ||
		    (pic.format != proto::kFmtJpeg && pic.format != proto::kFmtH264) ||
		    pic.w == 0 || pic.h == 0)
			return r;
		pic.data.reset(new (std::nothrow) uint8_t[pic.size]);
		if (!pic.data)
			return r;
		cur_ = std::move(pic);
		active_ = true;
		r.rx = Rx::Ok;
		return r;
	}

	// VIDEO_DATA payload. Bad = more bytes than announced (dropped).
	Result data(const uint8_t *p, unsigned n)
	{
		Result r;
		if (n < 2 || !active_ || proto::get_u16(p) != cur_.id) {
			r.rx = Rx::Ignored;
			return r;
		}
		unsigned len = n - 2;
		if (len > cur_.size - have_) {
			r.rx = Rx::Bad;
			r.id = (int)cur_.id;
			reset();
			return r;
		}
		std::memcpy(cur_.data.get() + have_, p + 2, len);
		have_ += len;
		r.rx = have_ == cur_.size ? Rx::Done : Rx::Ok;
		return r;
	}

	// After Rx::Done: hand the picture over.
	Picture take()
	{
		Picture pic = std::move(cur_);
		reset();
		return pic;
	}

	// Drops an incomplete picture.
	void reset()
	{
		cur_ = Picture();
		active_ = false;
		have_ = 0;
	}

	bool active() const { return active_; }

private:
	uint32_t max_bytes_;
	bool     active_ = false;  // a picture is being assembled
	Picture  cur_;
	uint32_t have_ = 0;        // bytes of cur_ received
};

#endif

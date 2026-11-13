// frame.h - the Android Auto Protocol (AAP) frame layer: what goes over the
// accessory's bulk pipes, both ways, once AOA is up.
//
//   frame = channel u8 | flags u8 | size u16 BE | [total u32 BE] | payload
//
//   flags  bit 0  FIRST   first frame of a message
//          bit 1  LAST    last frame of a message (FIRST|LAST = the whole
//                         message in one frame, "bulk")
//          bit 2  CONTROL a control message (CHANNEL_OPEN_*) on a service
//                         channel; channel 0's own messages don't set it
//          bit 3  ENCRYPTED  payload is TLS records
//   total  only on a FIRST frame that is not also LAST: the size of the
//          whole message (plaintext)
//   size   of this frame's payload as sent (ciphertext when encrypted)
//
// A message (after reassembly and decryption) is a 16-bit BE message id
// followed by a protobuf, or raw bytes for some (version, TLS, media).
// Each frame of a message is encrypted on its own; frames of different
// channels may interleave (the phone's video and audio do).
//
// Pure C++, no I/O: tests/test_android_auto.cpp.
#ifndef ANDROID_AUTO_FRAME_H
#define ANDROID_AUTO_FRAME_H

#include <cstddef>
#include "types.h"
#include <cstring>
#include <vector>

#include "usb_chunk.h"

namespace aap {

constexpr u8 kFirst = 1 << 0;
constexpr u8 kLast = 1 << 1;
constexpr u8 kBulk = kFirst | kLast;
constexpr u8 kControl = 1 << 2;
constexpr u8 kEncrypted = 1 << 3;

// Largest plaintext per frame the head unit sends (aasdk's
// cMaxFramePayloadSize); the phone uses the same.
constexpr usize kMaxFramePlain = 0x4000;
// Largest frame payload accepted from the phone: 16 KB of plaintext plus
// TLS record overhead, with room to spare.
constexpr usize kMaxFramePayload = 0x4000 + 2048;

struct Frame {
	u8        channel;
	u8        flags;
	u32       total;       // FIRST-only frames, else 0
	const u8 *payload;
	usize         size;
};

inline usize header_size(u8 flags)
{
	return (flags & kBulk) == kFirst ? 8 : 4;
}

// One frame: header + payload into out (appended).
inline void put_frame(std::vector<u8> &out, u8 channel, u8 flags, u32 total,
                      const u8 *payload, usize size)
{
	out.push_back(channel);
	out.push_back(flags);
	out.push_back((u8)(size >> 8));
	out.push_back((u8)size);
	if (header_size(flags) == 8) {
		out.push_back((u8)(total >> 24));
		out.push_back((u8)(total >> 16));
		out.push_back((u8)(total >> 8));
		out.push_back((u8)total);
	}
	out.insert(out.end(), payload, payload + size);
}

// Splits a byte stream (bulk IN reads, any boundaries) into frames.
class Decoder {
public:
	enum class Result { Ok, Bad };

	// Calls on_frame(const Frame &) for every complete frame in the bytes
	// so far. Bad: a frame larger than kMaxFramePayload (the stream is out
	// of step; the session drops the link).
	template <class F>
	Result feed(const u8 *p, usize n, F &&on_frame)
	{
		buf_.insert(buf_.end(), p, p + n);
		usize off = 0;
		Result r = Result::Ok;
		for (;;) {
			usize avail = buf_.size() - off;
			if (avail < 4)
				break;
			const u8 *h = buf_.data() + off;
			usize hs = header_size(h[1]);
			usize size = (usize)h[2] << 8 | h[3];
			if (size > kMaxFramePayload) {
				r = Result::Bad;
				off = buf_.size();
				break;
			}
			if (avail < hs + size)
				break;
			Frame f;
			f.channel = h[0];
			f.flags = h[1];
			f.total = hs == 8 ? (u32)h[4] << 24 | (u32)h[5] << 16 |
			                    (u32)h[6] << 8 | h[7] : 0;
			f.payload = h + hs;
			f.size = size;
			on_frame(f);
			off += hs + size;
		}
		buf_.erase(buf_.begin(), buf_.begin() + (isize)off);
		return r;
	}
	void reset() { buf_.clear(); }
	usize pending() const { return buf_.size(); }

private:
	std::vector<u8> buf_;
};

// Reassembles the frames of each channel into messages (after decryption).
class Assembler {
public:
	enum class Result { Partial, Done, Bad };

	// plain = this frame's plaintext. Done: message(channel) holds the
	// whole message, until the next add() on that channel.
	Result add(u8 channel, u8 flags, const u8 *plain, usize n)
	{
		Chan &c = ch_[channel];
		if (flags & kFirst) {
			c.msg.clear();
			c.open = true;
		} else if (!c.open) {
			return Result::Bad;              // MIDDLE / LAST without a FIRST
		}
		if (c.msg.size() + n > kMaxMessage) {
			c.open = false;
			return Result::Bad;
		}
		c.msg.insert(c.msg.end(), plain, plain + n);
		if (flags & kLast) {
			c.open = false;
			return Result::Done;
		}
		return Result::Partial;
	}
	const std::vector<u8> &message(u8 channel) const { return ch_[channel].msg; }
	void reset()
	{
		for (Chan &c : ch_) {
			c.msg.clear();
			c.open = false;
		}
	}

	// A video frame at 800x480 stays far below this.
	static constexpr usize kMaxMessage = 4u << 20;

private:
	struct Chan {
		std::vector<u8> msg;
		bool                 open = false;
	};
	Chan ch_[256];
};

// Message id (16-bit BE) at the start of a message; false if too short.
inline bool message_id(const std::vector<u8> &m, u16 &id)
{
	if (m.size() < 2)
		return false;
	id = (u16)(m[0] << 8 | m[1]);
	return true;
}

} // namespace aap

#endif

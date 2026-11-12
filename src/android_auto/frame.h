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
#include <cstdint>
#include <cstring>
#include <vector>

#include "usb_chunk.h"

namespace aap {

constexpr uint8_t kFirst = 1 << 0;
constexpr uint8_t kLast = 1 << 1;
constexpr uint8_t kBulk = kFirst | kLast;
constexpr uint8_t kControl = 1 << 2;
constexpr uint8_t kEncrypted = 1 << 3;

// Largest plaintext per frame the head unit sends (aasdk's
// cMaxFramePayloadSize); the phone uses the same.
constexpr size_t kMaxFramePlain = 0x4000;
// Largest frame payload accepted from the phone: 16 KB of plaintext plus
// TLS record overhead, with room to spare.
constexpr size_t kMaxFramePayload = 0x4000 + 2048;

struct Frame {
	uint8_t        channel;
	uint8_t        flags;
	uint32_t       total;       // FIRST-only frames, else 0
	const uint8_t *payload;
	size_t         size;
};

inline size_t header_size(uint8_t flags)
{
	return (flags & kBulk) == kFirst ? 8 : 4;
}

// One frame: header + payload into out (appended).
inline void put_frame(std::vector<uint8_t> &out, uint8_t channel, uint8_t flags, uint32_t total,
                      const uint8_t *payload, size_t size)
{
	out.push_back(channel);
	out.push_back(flags);
	out.push_back((uint8_t)(size >> 8));
	out.push_back((uint8_t)size);
	if (header_size(flags) == 8) {
		out.push_back((uint8_t)(total >> 24));
		out.push_back((uint8_t)(total >> 16));
		out.push_back((uint8_t)(total >> 8));
		out.push_back((uint8_t)total);
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
	Result feed(const uint8_t *p, size_t n, F &&on_frame)
	{
		buf_.insert(buf_.end(), p, p + n);
		size_t off = 0;
		Result r = Result::Ok;
		for (;;) {
			size_t avail = buf_.size() - off;
			if (avail < 4)
				break;
			const uint8_t *h = buf_.data() + off;
			size_t hs = header_size(h[1]);
			size_t size = (size_t)h[2] << 8 | h[3];
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
			f.total = hs == 8 ? (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 |
			                    (uint32_t)h[6] << 8 | h[7] : 0;
			f.payload = h + hs;
			f.size = size;
			on_frame(f);
			off += hs + size;
		}
		buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)off);
		return r;
	}
	void reset() { buf_.clear(); }
	size_t pending() const { return buf_.size(); }

private:
	std::vector<uint8_t> buf_;
};

// Reassembles the frames of each channel into messages (after decryption).
class Assembler {
public:
	enum class Result { Partial, Done, Bad };

	// plain = this frame's plaintext. Done: message(channel) holds the
	// whole message, until the next add() on that channel.
	Result add(uint8_t channel, uint8_t flags, const uint8_t *plain, size_t n)
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
	const std::vector<uint8_t> &message(uint8_t channel) const { return ch_[channel].msg; }
	void reset()
	{
		for (Chan &c : ch_) {
			c.msg.clear();
			c.open = false;
		}
	}

	// A video frame at 800x480 stays far below this.
	static constexpr size_t kMaxMessage = 4u << 20;

private:
	struct Chan {
		std::vector<uint8_t> msg;
		bool                 open = false;
	};
	Chan ch_[256];
};

// Message id (16-bit BE) at the start of a message; false if too short.
inline bool message_id(const std::vector<uint8_t> &m, uint16_t &id)
{
	if (m.size() < 2)
		return false;
	id = (uint16_t)(m[0] << 8 | m[1]);
	return true;
}

} // namespace aap

#endif

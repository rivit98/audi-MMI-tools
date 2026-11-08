// proto.h - audi_stream_receiver wire protocol, shared with the Android sender
// (android/app/src/main/java/mmi3g/audistreaming/Proto.kt must match). Pure,
// tests/test_proto.cpp.
//
// Every message on the AOA bulk pipes is one frame:
//
//   type u8 | len u16 LE | payload[len]          (len <= kMaxPayload)
//
//   HELLO       0x01  both   "who I am" (UTF-8). Sent by each side when its
//                            session starts; the peer answers HELLO_REPLY.
//   (0x02, 0x03)             retired: TEXT / ACK, text messages from the
//                            phone, dropped. Not to be reused;
//                            an old phone app's TEXT is logged and ignored.
//   HELLO_REPLY 0x04  both   "who I am", answer to HELLO (never answered)
//
// Video (a few fps of screen mirroring, one JPEG per picture):
//
//   VIDEO_CAPS  0x05  MMI    land_w u16, land_h u16, port_w u16, port_h u16,
//                            max_bytes u32, formats u8 (bit n-1 = format n:
//                            bit 0 JPEG, bit 2 H.264 - an MMI app without
//                            H.264 no bit 2; bit 1 was PNG, retired, not
//                            to be reused). The boxes a
//                            landscape / portrait picture has to fit in.
//                            With H.264 (22 bytes) followed by the
//                            H.264 limits: h264_max_w u16, h264_max_h u16,
//                            h264_fps u8, h264_max_frame u32 (the largest
//                            frame_bytes H264_CONFIG may ask for).
//                            Sent after HELLO and with HELLO_REPLY; a phone
//                            streams only to an MMI that sent it.
//   VIDEO_START 0x06  phone  id u16, w u16, h u16, format u8 (kFmt*),
//                            size u32 [, phone_ms u16]: a picture of size
//                            bytes follows ... phone_ms (for the perf log
//                            only) is what the phone spent
//                            on it: scaling + encoding. Longer payloads are
//                            fine, the rest is ignored.
//   VIDEO_DATA  0x07  phone  id u16 + the next bytes of it, in order
//   (0x08)                   retired: VIDEO_ACK, one per picture (id, status,
//                            decode_ms). Not to be reused.
//   VIDEO_STOP  0x09  phone  (empty) streaming ended, back to the status
//                            screen. The phone forgets what it had in flight.
//   VIDEO_DETAILS 0x0A phone show u8: 1 = the status bar (state, video
//                            stats) over a landscape picture, 0 = the
//                            picture alone (the MMI's default). Sent on
//                            the MMI's HELLO / HELLO_REPLY and whenever
//                            the user flips it. Portrait always has its
//                            status panel beside the picture.
//   VIDEO_ACKS  0x0D  MMI    shown u16, bad u16, dropped u16: this many more
//                            pictures are done, since the last VIDEO_ACKS,
//                            in the order they were sent. The phone counts
//                            its pictures in flight down by the sum. A JPEG
//                            goes out alone, at once: shown = on screen,
//                            and the phone keeps at most two in flight.
//                            H.264 frames go out in batches (ack_batch.h).
//
// Hardware H.264 (docs/phone-mirroring.md 7.10): the
// unit's DM642 decodes, io-media plays, nothing is drawn by the CPU.
//
//   H264_CONFIG 0x0B  phone  fps u8, frame_bytes u32, sps_len u16, sps,
//                            pps_len u16, pps [, vcap_mode u8]. The encoder's SPS and PPS
//                            (NAL units, header byte first, no start code;
//                            MediaCodec's csd-0 / csd-1 without it). fps is
//                            the rate the stream is timed at (the phone
//                            should keep sending at it, repeating frames
//                            when the screen is still: the MMI fills gaps
//                            with skip frames, but late). frame_bytes: no
//                            frame will be bigger (4-byte length per NAL
//                            unit included); every frame is padded to it,
//                            so it is also the bandwidth: frame_bytes x
//                            fps. Picture at most h264_max_w x h264_max_h
//                            (VIDEO_CAPS), Baseline, level 3.0 at most.
//                            Sent before the first frame and whenever the
//                            encoder is set up again (size, rotation). The
//                            same config again changes nothing; a new one
//                            restarts the stream (~1-2 s black).
//                            vcap_mode (kVcap*): VCAP 0's field mode for
//                            this stream. Absent = kVcapKeep.
//   (VIDEO_START / VIDEO_DATA with format kFmtH264: one encoded frame,
//   Annex-B (start codes), as MediaCodec hands it out; w/h the stream's
//   size. Counted in VIDEO_ACKS on arrival - shown: in the stream, dropped:
//   waiting for an IDR, bad: refused (too big, no config, ...). The first
//   frame after a config must be an IDR.)
//   H264_STATUS 0x0C  MMI    code u8 (kH264*):
//                            PLAYING  config accepted, the stream starts
//                                     at the next IDR
//                            SYNC     send an IDR now (start, or a frame
//                                     was dropped and the ones after it
//                                     can't be decoded)
//                            REFUSED  config unusable (log says why): send
//                                     JPEG pictures instead
//                            CONFIG   send H264_CONFIG again (in-band SPS
//                                     / PPS differ, or the stream ended)
//
// Frames may be split or merged arbitrarily across USB transfers; the
// decoder reassembles them. A frame announcing more than kMaxPayload is
// skipped (its bytes are counted off) so the stream stays in sync.
#ifndef AUDI_STREAM_PROTO_H
#define AUDI_STREAM_PROTO_H

#include <cstdint>
#include <cstring>
#include <optional>

namespace proto {

enum Type : uint8_t {
	kHello = 0x01,
	kHelloReply = 0x04,
	kVideoCaps = 0x05,
	kVideoStart = 0x06,
	kVideoData = 0x07,
	kVideoStop = 0x09,
	kVideoDetails = 0x0A,
	kH264Config = 0x0B,
	kH264Status = 0x0C,
	kVideoAcks = 0x0D,
};

constexpr unsigned kFmtJpeg = 1;
constexpr unsigned kFmtH264 = 3;

// H264_STATUS code
constexpr uint8_t kH264Playing = 0;
constexpr uint8_t kH264Sync = 1;
constexpr uint8_t kH264Refused = 2;
constexpr uint8_t kH264ConfigWanted = 3;

// H264_CONFIG vcap_mode
constexpr uint8_t kVcapKeep = 0;    // leave the mode as it is
constexpr uint8_t kVcapBob = 1;
constexpr uint8_t kVcapWeave = 2;
constexpr uint8_t kVcapOddOnly = 3;

// How a picture ended: one of VIDEO_ACKS's counts.
enum class PicStatus {
	Shown,
	Bad,      // not decodable, or bigger than its box
	Dropped,  // superseded or cut short before shown
};

constexpr unsigned kVideoCapsLen = 22;   // 13 without the H.264 limits
constexpr unsigned kH264ConfigMin = 11;  // fps, frame_bytes, two lengths, 1-byte SPS/PPS
constexpr unsigned kVideoStartLen = 11;  // the least; 13 with phone_ms
constexpr unsigned kVideoAcksLen = 6;
constexpr unsigned kVideoDetailsLen = 1;

constexpr unsigned kHdr = 3;
constexpr unsigned kMaxPayload = 2048;
constexpr unsigned kMaxFrame = kHdr + kMaxPayload;

inline uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

inline uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

inline void put_u16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8 & 0xff);
}

inline void put_u32(uint8_t *p, uint32_t v)
{
	put_u16(p, v & 0xffff);
	put_u16(p + 2, v >> 16);
}

inline const char *fmt_name(unsigned format)
{
	return format == kFmtH264 ? "H.264" : "JPEG";
}

// Encodes one frame into out[cap]. Returns its size, or 0 if len is over
// kMaxPayload or the frame does not fit in cap.
inline unsigned encode(uint8_t *out, unsigned cap, uint8_t type, const void *payload, unsigned len)
{
	if (len > kMaxPayload || kHdr + len > cap)
		return 0;
	out[0] = type;
	put_u16(out + 1, len);
	if (len > 0)
		std::memcpy(out + kHdr, payload, len);
	return kHdr + len;
}

// Reassembles frames from arbitrary chunks.
class Decoder {
public:
	// Feed n received bytes; on_frame(type, payload, len) runs once per
	// complete frame, in order.
	template <class OnFrame>
	void feed(const uint8_t *data, unsigned n, OnFrame &&on_frame)
	{
		while (n > 0) {
			if (skip_ > 0) {                 // dropping an oversized frame
				unsigned take = n < skip_ ? n : skip_;
				skip_ -= take;
				data += take;
				n -= take;
				continue;
			}
			unsigned want = have_ < kHdr ? kHdr - have_ : kHdr + get_u16(buf_ + 1) - have_;
			unsigned take = n < want ? n : want;
			std::memcpy(buf_ + have_, data, take);
			have_ += take;
			data += take;
			n -= take;
			if (have_ < kHdr)
				continue;

			unsigned len = get_u16(buf_ + 1);
			if (len > kMaxPayload) {
				oversized_++;
				skip_ = len;
				have_ = 0;
				continue;
			}
			if (have_ == kHdr + len) {
				on_frame(buf_[0], buf_ + kHdr, len);
				have_ = 0;
			}
		}
	}

	unsigned oversized() const { return oversized_; }  // frames dropped for being too big

private:
	uint8_t  buf_[kMaxFrame];
	unsigned have_ = 0;       // bytes of the current frame in buf_
	unsigned skip_ = 0;       // payload bytes of an oversized frame still to drop
	unsigned oversized_ = 0;
};

// H264_CONFIG payload; sps / pps point into it.
struct H264Config {
	unsigned       fps;
	uint32_t       frame_bytes;
	const uint8_t *sps, *pps;
	unsigned       sps_len, pps_len;
	uint8_t        vcap_mode;      // kVcap*; an unknown value reads as kVcapKeep
};

// Nothing if malformed (short, lengths past the end, an empty SPS / PPS, fps 0).
inline std::optional<H264Config> parse_h264_config(const uint8_t *p, unsigned n)
{
	if (n < kH264ConfigMin)
		return std::nullopt;
	H264Config c;
	c.fps = p[0];
	c.frame_bytes = get_u32(p + 1);
	c.sps_len = get_u16(p + 5);
	unsigned at = 7;
	if (c.sps_len == 0 || at + c.sps_len + 2 > n)
		return std::nullopt;
	c.sps = p + at;
	at += c.sps_len;
	c.pps_len = get_u16(p + at);
	at += 2;
	if (c.pps_len == 0 || at + c.pps_len > n)
		return std::nullopt;
	c.pps = p + at;
	at += c.pps_len;
	c.vcap_mode = at < n && p[at] <= kVcapOddOnly ? p[at] : kVcapKeep;
	if (c.fps == 0)
		return std::nullopt;
	return c;
}

} // namespace proto

#endif

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
// Video (screen mirroring, one H.264 frame per "picture"):
//
//   VIDEO_CAPS  0x05  MMI    land_w u16, land_h u16, port_w u16, port_h u16,
//                            max_bytes u32, formats u8 (bit n-1 = format n:
//                            bit 2 H.264; bit 0 was JPEG and bit 1 PNG,
//                            both retired, not to be reused). The boxes
//                            and max_bytes are what the retired JPEG
//                            pictures had to fit in: still sent (the
//                            screen and its left half, the H.264 frame
//                            cap), unused. Followed by the
//                            H.264 limits: h264_max_w u16, h264_max_h u16,
//                            h264_fps u8, h264_max_frame u32 (the largest
//                            frame_bytes H264_CONFIG may ask for).
//                            Sent after HELLO and with HELLO_REPLY; a phone
//                            streams only to an MMI that sent it.
//   VIDEO_START 0x06  phone  id u16, w u16, h u16, format u8 (kFmtH264),
//                            size u32 [, phone_ms u16]: a frame of size
//                            bytes follows. phone_ms (the phone's encode
//                            time) is ignored, like anything after it.
//   VIDEO_DATA  0x07  phone  id u16 + the next bytes of it, in order
//   (0x08)                   retired: VIDEO_ACK, one per picture (id, status,
//                            decode_ms). Not to be reused.
//   VIDEO_STOP  0x09  phone  (empty) streaming ended, back to the status
//                            screen. The phone forgets what it had in flight.
//   (0x0A)                   retired: VIDEO_DETAILS, a status bar over a
//                            landscape picture. Not to be reused.
//   VIDEO_ACKS  0x0D  MMI    shown u16, bad u16, dropped u16: this many more
//                            pictures are done, since the last VIDEO_ACKS,
//                            in the order they were sent. The phone counts
//                            its pictures in flight down by the sum. Sent
//                            in batches (ack_batch.h); a bad or cut-short
//                            frame at once.
//   STREAM_SETTINGS 0x0E phone  ASCII "key=value key=value ...": the phone
//                            app's streaming settings (format, max fps, bit
//                            rate, ...) and its uptime, for the MMI's log
//                            only. Sent when streaming starts on a link and
//                            whenever a setting changes. An older MMI logs
//                            it as an unknown type and ignores it.
//
// Hardware H.264 (docs/phone-mirroring.md 7.10): the
// unit's DM642 decodes, io-media plays, nothing is drawn by the CPU.
//
//   H264_CONFIG 0x0B  phone  fps u8, frame_bytes u32, sps_len u16, sps,
//                            pps_len u16, pps. The encoder's SPS and PPS
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
//                            Bytes after the PPS are ignored (an older phone
//                            app sent vcap_mode u8 there: retired, VCAP 0
//                            always weaves, docs/phone-mirroring.md 7.4).
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
//                            REFUSED  config unusable (log says why): the
//                                     phone pauses and tries a config
//                                     again later
//                            CONFIG   send H264_CONFIG again (in-band SPS
//                                     / PPS differ, or the stream ended)
//
// Phone audio (the phone's own playback, captured and SBC-encoded on the
// phone; docs/bt-audio-path.md §5). The MMI checks and logs it; it does not
// play it yet:
//
//   AUDIO_CAPS   0x0F MMI    codecs u8 (bit 0 SBC), plays u8 (0: the MMI
//                            only checks and logs the stream). Sent with
//                            VIDEO_CAPS; a phone sends audio only to an MMI
//                            that sent it.
//   AUDIO_CONFIG 0x10 phone  codec u8 (1 SBC), rate u32 (Hz), channels u8,
//                            blocks u8, subbands u8, mode u8 (0 mono, 1
//                            dual, 2 stereo, 3 joint), loudness u8 (1
//                            loudness, 0 SNR allocation), bitpool u8,
//                            frame_len u16: every frame's size. Starts or
//                            restarts the stream; frames count from 0.
//   AUDIO_DATA   0x11 phone  seq u32 (the number of its first frame since
//                            AUDIO_CONFIG), phone_ms u32 (the phone's clock,
//                            ms, when that frame's first sample was
//                            captured), then whole SBC frames (A2DP's
//                            frame format, header + CRC, no RTP).
//   AUDIO_STOP   0x12 phone  (empty) the phone stopped capturing.
//
// Frames may be split or merged arbitrarily across USB transfers; the
// decoder reassembles them. A frame announcing more than kMaxPayload is
// skipped (its bytes are counted off) so the stream stays in sync.
#ifndef AUDI_STREAM_PROTO_H
#define AUDI_STREAM_PROTO_H

#include "types.h"
#include <cstring>
#include <optional>

namespace proto {

enum Type : u8 {
	kHello = 0x01,
	kHelloReply = 0x04,
	kVideoCaps = 0x05,
	kVideoStart = 0x06,
	kVideoData = 0x07,
	kVideoStop = 0x09,
	kH264Config = 0x0B,
	kH264Status = 0x0C,
	kVideoAcks = 0x0D,
	kStreamSettings = 0x0E,
	kAudioCaps = 0x0F,
	kAudioConfig = 0x10,
	kAudioData = 0x11,
	kAudioStop = 0x12,
};

constexpr u8 kAudioCodecSbc = 1;
constexpr unsigned kAudioCapsLen = 2;
constexpr unsigned kAudioConfigLen = 13;
constexpr unsigned kAudioDataHdr = 8;     // seq u32, phone_ms u32

// (1 JPEG and 2 PNG: retired, not to be reused)
constexpr unsigned kFmtH264 = 3;

// H264_STATUS code
constexpr u8 kH264Playing = 0;
constexpr u8 kH264Sync = 1;
constexpr u8 kH264Refused = 2;
constexpr u8 kH264ConfigWanted = 3;

// How a picture ended: one of VIDEO_ACKS's counts.
enum class PicStatus {
	Shown,
	Bad,      // refused (too big, no config, ...)
	Dropped,  // superseded or cut short before shown
};

constexpr unsigned kVideoCapsLen = 22;   // 13 without the H.264 limits
constexpr unsigned kH264ConfigMin = 11;  // fps, frame_bytes, two lengths, 1-byte SPS/PPS
constexpr unsigned kVideoStartLen = 11;  // the least; 13 with phone_ms
constexpr unsigned kVideoAcksLen = 6;

constexpr unsigned kHdr = 3;
constexpr unsigned kMaxPayload = 2048;
constexpr unsigned kMaxFrame = kHdr + kMaxPayload;

inline u16 get_u16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

inline u32 get_u32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

inline void put_u16(u8 *p, unsigned v)
{
	p[0] = (u8)(v & 0xff);
	p[1] = (u8)(v >> 8 & 0xff);
}

inline void put_u32(u8 *p, u32 v)
{
	put_u16(p, v & 0xffff);
	put_u16(p + 2, v >> 16);
}

// Encodes one frame into out[cap]. Returns its size, or 0 if len is over
// kMaxPayload or the frame does not fit in cap.
inline unsigned encode(u8 *out, unsigned cap, u8 type, const void *payload, unsigned len)
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
	void feed(const u8 *data, unsigned n, OnFrame &&on_frame)
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
	u8  buf_[kMaxFrame];
	unsigned have_ = 0;       // bytes of the current frame in buf_
	unsigned skip_ = 0;       // payload bytes of an oversized frame still to drop
	unsigned oversized_ = 0;
};

// H264_CONFIG payload; sps / pps point into it.
struct H264Config {
	unsigned       fps;
	u32       frame_bytes;
	const u8 *sps, *pps;
	unsigned       sps_len, pps_len;
};

// Nothing if malformed (short, lengths past the end, an empty SPS / PPS, fps 0).
inline std::optional<H264Config> parse_h264_config(const u8 *p, unsigned n)
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
	if (c.fps == 0)
		return std::nullopt;
	return c;
}

// AUDIO_CONFIG payload.
struct AudioConfig {
	u8  codec = 0;
	u32 rate = 0;
	u8  channels = 0, blocks = 0, subbands = 0, mode = 0, loudness = 0, bitpool = 0;
	u16 frame_len = 0;
};

// Nothing if short or not SBC-shaped (codec, channels, blocks, subbands,
// mode out of range, frame_len 0).
inline std::optional<AudioConfig> parse_audio_config(const u8 *p, unsigned n)
{
	if (n < kAudioConfigLen)
		return std::nullopt;
	AudioConfig c;
	c.codec = p[0];
	c.rate = get_u32(p + 1);
	c.channels = p[5];
	c.blocks = p[6];
	c.subbands = p[7];
	c.mode = p[8];
	c.loudness = p[9];
	c.bitpool = p[10];
	c.frame_len = get_u16(p + 11);
	bool blocks_ok = c.blocks == 4 || c.blocks == 8 || c.blocks == 12 || c.blocks == 16;
	if (c.codec != kAudioCodecSbc || c.rate == 0 || c.channels < 1 || c.channels > 2 ||
	    !blocks_ok || (c.subbands != 4 && c.subbands != 8) || c.mode > 3 || c.frame_len == 0 ||
	    (c.mode == 0) != (c.channels == 1))
		return std::nullopt;
	return c;
}

inline void encode_audio_config(const AudioConfig &c, u8 out[kAudioConfigLen])
{
	out[0] = c.codec;
	put_u32(out + 1, c.rate);
	out[5] = c.channels;
	out[6] = c.blocks;
	out[7] = c.subbands;
	out[8] = c.mode;
	out[9] = c.loudness;
	out[10] = c.bitpool;
	put_u16(out + 11, c.frame_len);
}

} // namespace proto

#endif

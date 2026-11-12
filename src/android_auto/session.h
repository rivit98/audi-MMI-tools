// session.h - one Android Auto session, head unit side: everything between
// "the accessory's bulk pipes are open" and "the phone has gone".
//
//   1. VERSION_REQUEST (1.1) -> the phone's VERSION_RESPONSE
//   2. TLS handshake in ENCAPSULATED_SSL messages (tls.h), then
//      AUTH_COMPLETE; everything after this is encrypted
//   3. SERVICE_DISCOVERY_REQUEST (the phone's name) -> our services:
//      video sink (H.264 Baseline 800x480 30 fps), optionally three audio
//      sinks (media 48 kHz stereo, guidance + system 16 kHz mono), sensors
//      (driving status, night mode), input (touch screen 800x480 + keys)
//   4. the phone opens each channel (CHANNEL_OPEN_REQUEST -> OK), sets up
//      each media sink (SETUP -> CONFIG ready, max 1 unacked), starts it
//      and streams; every media message is acked
//   5. pings both ways, audio / navigation / video focus always granted,
//      sensors answered with fixed values: parked (unrestricted), day
//
// Pure C++ over tls.h: no USB, no threads. The app feeds it the bytes from
// bulk IN (rx), calls tick() regularly and gets back what to write to bulk
// OUT and the media through Host. One thread at a time.
//
// Message ids and field numbers: aasdk's aap_protobuf/*.proto
// (docs/android-auto.md).
#ifndef ANDROID_AUTO_SESSION_H
#define ANDROID_AUTO_SESSION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frame.h"
#include "tls.h"

namespace aa {

// Our channel numbers, as advertised in the service discovery response.
constexpr uint8_t kChControl = 0;
constexpr uint8_t kChInput = 1;
constexpr uint8_t kChSensor = 2;
constexpr uint8_t kChVideo = 3;
constexpr uint8_t kChMediaAudio = 4;
constexpr uint8_t kChGuidanceAudio = 5;
constexpr uint8_t kChSystemAudio = 6;
constexpr uint8_t kChMic = 7;           // advertised always: the phone ends the session without one

enum class Phase {
	Idle,        // not started
	Version,     // VERSION_REQUEST sent
	Handshake,   // TLS
	Discovery,   // AUTH_COMPLETE sent, waiting for SERVICE_DISCOVERY_REQUEST
	Running,     // services advertised; channels open as the phone wants
	Ended,       // the phone said bye-bye
	Failed,      // see Stats::error
};
const char *phase_name(Phase p);

struct Config {
	uint16_t    version_major = 1;
	uint16_t    version_minor = 1;
	bool        audio = true;          // advertise the three audio sinks
	unsigned    dpi = 140;
	unsigned    ping_ms = 1000;
	std::string display_name = "MMI 3G+";
};

struct Stats {
	std::string phone;               // device_name from the discovery request
	std::string label;               // label_text
	std::string suite;               // TLS version + cipher suite
	std::string error;               // why Failed
	uint32_t    open_channels = 0;   // bit n: channel n opened
	int32_t     video_session = -1;  // from START, -1 none yet
	bool        video_started = false;
	uint64_t    video_frames = 0, video_bytes = 0, video_configs = 0;
	uint64_t    audio_bytes[3] = {};  // media, guidance, system
	uint64_t    rx_bytes = 0, tx_bytes = 0, rx_frames = 0;
	unsigned    pings_sent = 0, pongs = 0, pings_answered = 0;
	unsigned    last_rtt_ms = 0;
	unsigned    handshake_ms = 0;    // the TLS handshake, from the first ClientHello
	unsigned    unknown = 0;         // messages we had no handler for
};

class Host {
public:
	virtual ~Host() = default;
	// Bytes for bulk OUT, in order.
	virtual void send(const uint8_t *p, size_t n) = 0;
	virtual void phase(Phase, const std::string & /*detail*/) {}
	// H.264 Annex-B: codec config (SPS/PPS) or a frame, ts in us (0 if none).
	virtual void video(const uint8_t *, size_t, uint64_t /*ts_us*/, bool /*config*/) {}
	// PCM on one of the audio channels.
	virtual void audio(uint8_t /*channel*/, const uint8_t *, size_t) {}
	// Milliseconds, monotonic (pings, handshake time).
	virtual unsigned now_ms() = 0;
};

class Session {
public:
	Session(const Config &cfg, Host &host);

	// Sets up TLS with the head unit certificate and sends VERSION_REQUEST.
	// false: Failed (Stats::error says why).
	bool start(const std::string &cert_pem, const std::string &key_pem);
	// Bytes read from bulk IN, any boundaries.
	void rx(const uint8_t *p, size_t n);
	// Pings; call every ~100 ms or so.
	void tick();
	// Tell the phone we are going (BYEBYE_REQUEST), if the session is up.
	void bye();

	Phase phase() const { return phase_; }
	const Stats &stats() const { return stats_; }

	// For tests: the service discovery response we send.
	std::vector<uint8_t> discovery_response() const;

private:
	void set_phase(Phase p, const std::string &detail);
	void fail(const std::string &why);
	void send_msg(uint8_t ch, uint8_t flags, uint16_t id, const uint8_t *body, size_t n);
	void send_msg(uint8_t ch, uint8_t flags, uint16_t id, const std::vector<uint8_t> &body)
	{
		send_msg(ch, flags, id, body.data(), body.size());
	}
	void on_frame(const aap::Frame &f);
	void on_message(uint8_t ch, uint8_t flags, const std::vector<uint8_t> &m);
	void on_control(uint16_t id, const uint8_t *p, size_t n);
	void on_handshake(const uint8_t *p, size_t n);
	void on_channel_open(uint8_t ch, const uint8_t *p, size_t n);
	void on_media(uint8_t ch, uint16_t id, const uint8_t *p, size_t n);
	void on_sensor(uint16_t id, const uint8_t *p, size_t n);
	void on_input(uint16_t id, const uint8_t *p, size_t n);
	void send_video_focus();
	void ack_media(uint8_t ch);
	bool channel_known(uint8_t ch) const;
	void log_unknown(uint8_t ch, uint16_t id, size_t n);

	Config         cfg_;
	Host          &host_;
	Tls            tls_;
	aap::Decoder   decoder_;
	aap::Assembler assembler_;
	Phase          phase_ = Phase::Idle;
	Stats          stats_;
	int32_t        media_session_[256];
	unsigned       handshake_t0_ = 0;
	unsigned       last_ping_ = 0;
	unsigned       last_pong_ = 0;
	bool           pong_warned_ = false;
	std::vector<uint8_t> plain_;     // scratch: a frame's plaintext
};

} // namespace aa

#endif

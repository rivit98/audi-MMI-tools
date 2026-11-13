// session.cpp - see session.h.
#include "session.h"
#include "types.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "log.h"
}

#include "pb.h"

namespace aa {

namespace {

// control channel (ControlMessageType.proto)
enum : u16 {
	kVersionRequest = 1,
	kVersionResponse = 2,
	kEncapsulatedSsl = 3,
	kAuthComplete = 4,
	kServiceDiscoveryRequest = 5,
	kServiceDiscoveryResponse = 6,
	kChannelOpenRequest = 7,
	kChannelOpenResponse = 8,
	kChannelCloseNotification = 9,
	kPingRequest = 11,
	kPingResponse = 12,
	kNavFocusRequest = 13,
	kNavFocusNotification = 14,
	kByeByeRequest = 15,
	kByeByeResponse = 16,
	kVoiceSessionNotification = 17,
	kAudioFocusRequest = 18,
	kAudioFocusNotification = 19,
	kBatteryStatusNotification = 23,
	kCallAvailabilityStatus = 24,
};

// media sink channels (MediaMessageId.proto)
enum : u16 {
	kMediaData = 0x0000,         // u64 BE timestamp + data
	kMediaCodecConfig = 0x0001,  // data
	kMediaSetup = 0x8000,
	kMediaStart = 0x8001,
	kMediaStop = 0x8002,
	kMediaConfig = 0x8003,
	kMediaAck = 0x8004,
	kMicRequest = 0x8005,
	kMicResponse = 0x8006,
	kVideoFocusRequest = 0x8007,
	kVideoFocusNotification = 0x8008,
};

// sensor channel (SensorMessageId.proto)
enum : u16 {
	kSensorRequest = 0x8001,
	kSensorResponse = 0x8002,
	kSensorBatch = 0x8003,
};

// input channel (InputMessageId.proto)
enum : u16 {
	kKeyBindingRequest = 0x8002,
	kKeyBindingResponse = 0x8003,
};

// SensorType.proto
constexpr unsigned kSensorNightMode = 10;
constexpr unsigned kSensorDrivingStatus = 13;

// MediaCodecType.proto / VideoCodecResolutionType / VideoFrameRateType /
// AudioStreamType
constexpr unsigned kCodecPcm = 1;
constexpr unsigned kCodecH264Bp = 3;
constexpr unsigned kVideo800x480 = 1;
constexpr unsigned kFps30 = 2;
constexpr unsigned kAudioGuidance = 1;
constexpr unsigned kAudioSystem = 2;
constexpr unsigned kAudioMedia = 3;

// KeyCode.proto: what the MMI could send one day (main knob, its push,
// the buttons around it).
const std::vector<i32> kKeycodes = {
	3,       // HOME
	4,       // BACK
	19, 20, 21, 22, 23,  // DPAD up / down / left / right / center
	84,      // SEARCH (voice)
	85, 87, 88,  // MEDIA play-pause / next / previous
	65536,   // ROTARY_CONTROLLER
};

constexpr unsigned kPongWarnMs = 10000;

u16 be16(const u8 *p)
{
	return (u16)(p[0] << 8 | p[1]);
}

u64 be64(const u8 *p)
{
	u64 v = 0;
	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

const char *channel_name(u8 ch)
{
	switch (ch) {
	case kChControl:       return "control";
	case kChInput:         return "input";
	case kChSensor:        return "sensor";
	case kChVideo:         return "video";
	case kChMediaAudio:    return "media audio";
	case kChGuidanceAudio: return "guidance audio";
	case kChSystemAudio:   return "system audio";
	case kChMic:           return "microphone";
	}
	return "?";
}

int audio_index(u8 ch)
{
	switch (ch) {
	case kChMediaAudio:    return 0;
	case kChGuidanceAudio: return 1;
	case kChSystemAudio:   return 2;
	}
	return -1;
}

pb::Writer audio_sink(unsigned type, unsigned rate, unsigned channels)
{
	pb::Writer conf, sink;
	conf.u(1, rate).u(2, 16).u(3, channels);           // AudioConfiguration
	sink.u(1, kCodecPcm).u(2, type).msg(3, conf).b(5, true);
	return sink;
}

pb::Writer service(unsigned id, unsigned field, const pb::Writer &body)
{
	pb::Writer s;
	s.u(1, id).msg(field, body);
	return s;
}

} // namespace

const char *phase_name(Phase p)
{
	switch (p) {
	case Phase::Idle:      return "idle";
	case Phase::Version:   return "version";
	case Phase::Handshake: return "TLS handshake";
	case Phase::Discovery: return "service discovery";
	case Phase::Running:   return "running";
	case Phase::Ended:     return "ended";
	case Phase::Failed:    return "failed";
	}
	return "?";
}

Session::Session(const Config &cfg, Host &host) : cfg_(cfg), host_(host)
{
	for (i32 &s : media_session_)
		s = -1;
}

void Session::set_phase(Phase p, const std::string &detail)
{
	phase_ = p;
	log_msg("aa: phase %s%s%s", phase_name(p), detail.empty() ? "" : ": ", detail.c_str());
	host_.phase(p, detail);
}

void Session::fail(const std::string &why)
{
	if (phase_ == Phase::Failed)
		return;
	stats_.error = why;
	set_phase(Phase::Failed, why);
}

bool Session::channel_known(u8 ch) const
{
	switch (ch) {
	case kChInput:
	case kChSensor:
	case kChVideo:
	case kChMic:
		return true;
	case kChMediaAudio:
	case kChGuidanceAudio:
	case kChSystemAudio:
		return cfg_.audio;
	}
	return false;
}

// --- out -----------------------------------------------------------------

void Session::send_msg(u8 ch, u8 flags, u16 id, const u8 *body, usize n)
{
	std::vector<u8> plain;
	plain.reserve(2 + n);
	plain.push_back((u8)(id >> 8));
	plain.push_back((u8)id);
	plain.insert(plain.end(), body, body + n);

	std::vector<u8> out, rec;
	usize total = plain.size();
	for (usize off = 0; off < total || total == 0;) {
		usize len = total - off < aap::kMaxFramePlain ? total - off : aap::kMaxFramePlain;
		u8 type = (off == 0 ? aap::kFirst : 0) | (off + len == total ? aap::kLast : 0);
		const u8 *payload = plain.data() + off;
		usize size = len;
		if (flags & aap::kEncrypted) {
			rec.clear();
			if (!tls_.encrypt(payload, len, rec)) {
				fail(tls_.error());
				return;
			}
			payload = rec.data();
			size = rec.size();
		}
		aap::put_frame(out, ch, (u8)(flags | type), (u32)total, payload, size);
		off += len;
		if (total == 0)
			break;
	}
	stats_.tx_bytes += out.size();
	host_.send(out.data(), out.size());
}

std::vector<u8> Session::discovery_response() const
{
	pb::Writer r;

	// video: H.264 Baseline 800x480 at 30 fps (MediaSinkService,
	// VideoConfiguration)
	pb::Writer vconf, video;
	vconf.u(1, kVideo800x480).u(2, kFps30).u(3, 0).u(4, 0).u(5, cfg_.dpi);
	video.u(1, kCodecH264Bp).msg(4, vconf).b(5, true);
	r.msg(1, service(kChVideo, 3, video));

	if (cfg_.audio) {
		r.msg(1, service(kChMediaAudio, 3, audio_sink(kAudioMedia, 48000, 2)));
		r.msg(1, service(kChGuidanceAudio, 3, audio_sink(kAudioGuidance, 16000, 1)));
		r.msg(1, service(kChSystemAudio, 3, audio_sink(kAudioSystem, 16000, 1)));
	}

	// microphone (MediaSourceService: PCM 16 kHz mono). Android Auto ends
	// the session with "No audio/mic" without one. We send no mic data.
	pb::Writer mconf, mic;
	mconf.u(1, 16000).u(2, 16).u(3, 1);
	mic.u(1, kCodecPcm).msg(2, mconf).b(3, false);
	r.msg(1, service(kChMic, 5, mic));

	// sensors (SensorSourceService: repeated Sensor{sensor_type})
	pb::Writer sensors, s1, s2;
	s1.u(1, kSensorDrivingStatus);
	s2.u(1, kSensorNightMode);
	sensors.msg(1, s1).msg(1, s2);
	r.msg(1, service(kChSensor, 2, sensors));

	// input (InputSourceService: keycodes, a touch screen the size of the video)
	pb::Writer input, touch;
	touch.u(1, 800).u(2, 480);
	input.packed(1, kKeycodes).msg(2, touch);
	r.msg(1, service(kChInput, 4, input));

	// who we are: the deprecated top-level fields for older phones,
	// HeadUnitInfo (17) for newer ones
	r.s(2, "Audi").s(3, "MMI 3G+").s(4, "2012").s(5, "0001");
	r.u(6, 0);                                      // DRIVER_POSITION_LEFT
	r.s(7, "mmi3g").s(8, "android_auto").s(9, "1").s(10, "0.1");
	r.b(11, false);
	r.s(14, cfg_.display_name);
	pb::Writer ping, conn, info;
	ping.u(1, 3000).u(2, cfg_.ping_ms).u(3, 200).u(4, 5);   // PingConfiguration
	conn.msg(1, ping);
	r.msg(16, conn);
	info.s(1, "Audi").s(2, "MMI 3G+").s(3, "2012").s(4, "0001")
	    .s(5, "mmi3g").s(6, "android_auto").s(7, "1").s(8, "0.1");
	r.msg(17, info);
	return r.buf;
}

// --- in ------------------------------------------------------------------

bool Session::start(const std::string &cert_pem, const std::string &key_pem)
{
	std::string err;
	if (!tls_.init(cert_pem, key_pem, &err)) {
		fail("TLS setup: " + err);
		return false;
	}
	u8 v[4] = {(u8)(cfg_.version_major >> 8), (u8)cfg_.version_major,
	                (u8)(cfg_.version_minor >> 8), (u8)cfg_.version_minor};
	char detail[48];
	std::snprintf(detail, sizeof(detail), "asked for protocol %u.%u", cfg_.version_major,
	              cfg_.version_minor);
	set_phase(Phase::Version, detail);
	send_msg(kChControl, 0, kVersionRequest, v, sizeof(v));
	return phase_ != Phase::Failed;
}

void Session::rx(const u8 *p, usize n)
{
	if (phase_ == Phase::Failed || phase_ == Phase::Idle)
		return;
	stats_.rx_bytes += n;
	auto r = decoder_.feed(p, n, [this](const aap::Frame &f) {
		if (phase_ != Phase::Failed)
			on_frame(f);
	});
	if (r == aap::Decoder::Result::Bad)
		fail("framing: a frame larger than the protocol allows (stream out of step)");
}

void Session::on_frame(const aap::Frame &f)
{
	stats_.rx_frames++;
	const u8 *plain = f.payload;
	usize n = f.size;
	if (f.flags & aap::kEncrypted) {
		if (!tls_.done()) {
			fail("an encrypted frame before the TLS handshake ended");
			return;
		}
		plain_.clear();
		if (!tls_.decrypt(f.payload, f.size, plain_)) {
			fail(tls_.error());
			return;
		}
		plain = plain_.data();
		n = plain_.size();
	}
	switch (assembler_.add(f.channel, f.flags, plain, n)) {
	case aap::Assembler::Result::Partial:
		return;
	case aap::Assembler::Result::Bad:
		log_msg("aa: channel %u: frame flags 0x%02x out of order or message too big, dropped",
		        f.channel, f.flags);
		return;
	case aap::Assembler::Result::Done:
		on_message(f.channel, f.flags, assembler_.message(f.channel));
		return;
	}
}

void Session::on_message(u8 ch, u8 flags, const std::vector<u8> &m)
{
	u16 id;
	if (!aap::message_id(m, id)) {
		log_msg("aa: channel %u: message of %zu bytes, no id", ch, m.size());
		return;
	}
	const u8 *p = m.data() + 2;
	usize n = m.size() - 2;
	if (ch == kChControl) {
		on_control(id, p, n);
		return;
	}
	if ((flags & aap::kControl) && id == kChannelOpenRequest) {
		on_channel_open(ch, p, n);
		return;
	}
	if (!channel_known(ch)) {
		log_unknown(ch, id, n);
		return;
	}
	if (ch == kChSensor)
		on_sensor(id, p, n);
	else if (ch == kChInput)
		on_input(id, p, n);
	else
		on_media(ch, id, p, n);
}

void Session::log_unknown(u8 ch, u16 id, usize n)
{
	stats_.unknown++;
	log_msg("aa: channel %u (%s): message 0x%04x, %zu bytes, not handled", ch, channel_name(ch), id, n);
}

void Session::on_control(u16 id, const u8 *p, usize n)
{
	u64 v = 0;
	pb::Writer w;

	switch (id) {
	case kVersionResponse: {
		if (n < 6) {
			fail("VERSION_RESPONSE too short");
			return;
		}
		unsigned major = be16(p), minor = be16(p + 2);
		i16 status = (i16)be16(p + 4);
		log_msg("aa: VERSION_RESPONSE %u.%u status %d", major, minor, status);
		// Phones answer non-zero statuses on sessions that work; only -1
		// (STATUS_NO_COMPATIBLE_VERSION) refuses (open-headunit's
		// AapVersionNegotiation.kt). Anything else: try TLS.
		if (status == -1) {
			fail("the phone refused protocol version " + std::to_string(cfg_.version_major) + "." +
			     std::to_string(cfg_.version_minor));
			return;
		}
		set_phase(Phase::Handshake, "phone speaks " + std::to_string(major) + "." + std::to_string(minor));
		handshake_t0_ = host_.now_ms();
		on_handshake(nullptr, 0);
		return;
	}
	case kEncapsulatedSsl:
		if (phase_ != Phase::Handshake) {
			log_msg("aa: TLS bytes outside the handshake (%zu), ignored", n);
			return;
		}
		on_handshake(p, n);
		return;
	case kServiceDiscoveryRequest: {
		stats_.phone = std::string(pb::get_str(p, n, 5));
		stats_.label = std::string(pb::get_str(p, n, 4));
		log_msg("aa: SERVICE_DISCOVERY_REQUEST from \"%s\" (label \"%s\"), %zu bytes",
		        stats_.phone.c_str(), stats_.label.c_str(), n);
		send_msg(kChControl, aap::kEncrypted, kServiceDiscoveryResponse, discovery_response());
		set_phase(Phase::Running, stats_.phone.empty() ? "services sent" : stats_.phone);
		last_ping_ = last_pong_ = host_.now_ms();
		return;
	}
	case kPingRequest:
		pb::get_u(p, n, 1, v);
		w.i(1, (i64)v);
		send_msg(kChControl, 0, kPingResponse, w.buf);    // plain, as aasdk
		stats_.pings_answered++;
		return;
	case kPingResponse:
		stats_.pongs++;
		last_pong_ = host_.now_ms();
		pong_warned_ = false;
		if (pb::get_u(p, n, 1, v))
			stats_.last_rtt_ms = (unsigned)(host_.now_ms() - (unsigned)v);
		return;
	case kAudioFocusRequest: {
		pb::get_u(p, n, 1, v);
		// RELEASE (4) -> LOSS (3), anything else -> GAIN (1), as openauto
		unsigned state = v == 4 ? 3 : 1;
		log_msg("aa: AUDIO_FOCUS_REQUEST type %u -> state %u", (unsigned)v, state);
		w.u(1, state).b(2, false);
		send_msg(kChControl, aap::kEncrypted, kAudioFocusNotification, w.buf);
		return;
	}
	case kNavFocusRequest:
		pb::get_u(p, n, 1, v);
		log_msg("aa: NAV_FOCUS_REQUEST type %u -> projected", (unsigned)v);
		w.u(1, 2);                                       // NAV_FOCUS_PROJECTED
		send_msg(kChControl, aap::kEncrypted, kNavFocusNotification, w.buf);
		return;
	case kByeByeRequest:
		pb::get_u(p, n, 1, v);
		log_msg("aa: BYEBYE_REQUEST reason %u", (unsigned)v);
		send_msg(kChControl, aap::kEncrypted, kByeByeResponse, nullptr, 0);
		set_phase(Phase::Ended, "the phone ended the session");
		return;
	case kByeByeResponse:
		set_phase(Phase::Ended, "bye-bye answered");
		return;
	case kChannelCloseNotification:
	case kVoiceSessionNotification:
	case kBatteryStatusNotification:
	case kCallAvailabilityStatus:
		log_msg("aa: control message %u (%zu bytes)", id, n);
		return;
	}
	log_unknown(kChControl, id, n);
}

void Session::on_handshake(const u8 *p, usize n)
{
	std::vector<u8> out;
	Tls::Step step = tls_.handshake(p, n, out);
	if (!out.empty())
		send_msg(kChControl, 0, kEncapsulatedSsl, out);
	if (step == Tls::Step::Failed) {
		fail("TLS " + tls_.error());
		return;
	}
	if (step == Tls::Step::Done) {
		stats_.handshake_ms = host_.now_ms() - handshake_t0_;
		stats_.suite = tls_.suite();
		log_msg("aa: TLS up in %u ms: %s", stats_.handshake_ms, stats_.suite.c_str());
		pb::Writer w;
		w.i(1, 0);                                       // STATUS_SUCCESS
		send_msg(kChControl, 0, kAuthComplete, w.buf);
		set_phase(Phase::Discovery, stats_.suite);
	}
}

void Session::on_channel_open(u8 ch, const u8 *p, usize n)
{
	u64 prio = 0, svc = 0;
	pb::get_u(p, n, 1, prio);
	pb::get_u(p, n, 2, svc);
	bool ok = channel_known(ch);
	log_msg("aa: CHANNEL_OPEN_REQUEST channel %u (%s) service %u priority %d: %s", ch,
	        channel_name(ch), (unsigned)svc, (int)pb::unzigzag(prio), ok ? "ok" : "refused");   // sint32
	pb::Writer w;
	w.i(1, ok ? 0 : -5);                                 // SUCCESS / INVALID_CHANNEL
	send_msg(ch, aap::kEncrypted | aap::kControl, kChannelOpenResponse, w.buf);
	if (ok)
		stats_.open_channels |= 1u << ch;
}

void Session::send_video_focus()
{
	pb::Writer w;
	w.u(1, 1).b(2, false);                               // VIDEO_FOCUS_PROJECTED, solicited
	send_msg(kChVideo, aap::kEncrypted, kVideoFocusNotification, w.buf);
}

void Session::ack_media(u8 ch)
{
	pb::Writer w;
	w.i(1, media_session_[ch]).u(2, 1);
	send_msg(ch, aap::kEncrypted, kMediaAck, w.buf);
}

void Session::on_media(u8 ch, u16 id, const u8 *p, usize n)
{
	u64 v = 0, idx = 0;
	pb::Writer w;
	int ai = audio_index(ch);

	switch (id) {
	case kMediaSetup:
		pb::get_u(p, n, 1, v);
		log_msg("aa: %s SETUP codec %u -> ready, 1 unacked", channel_name(ch), (unsigned)v);
		w.u(1, 2).u(2, 1).u(3, 0);                       // STATUS_READY, max_unacked 1, config 0
		send_msg(ch, aap::kEncrypted, kMediaConfig, w.buf);
		if (ch == kChVideo)
			send_video_focus();
		return;
	case kMediaStart:
		pb::get_u(p, n, 1, v);
		pb::get_u(p, n, 2, idx);
		media_session_[ch] = (i32)v;
		if (ch == kChVideo)
			stats_.video_session = (i32)v;
		log_msg("aa: %s START session %d config %u", channel_name(ch), (int)(i32)v, (unsigned)idx);
		return;
	case kMediaStop:
		log_msg("aa: %s STOP", channel_name(ch));
		return;
	case kMediaCodecConfig:
	case kMediaData: {
		u64 ts = 0;
		if (id == kMediaData) {
			if (n < 8) {
				log_msg("aa: %s data message of %zu bytes, too short", channel_name(ch), n);
				return;
			}
			ts = be64(p);
			p += 8;
			n -= 8;
		}
		if (ch == kChVideo) {
			bool config = id == kMediaCodecConfig;
			if (config) {
				stats_.video_configs++;
				log_msg("aa: video codec config, %zu bytes", n);
			} else {
				if (!stats_.video_started)
					log_msg("aa: first video frame, %zu bytes", n);
				stats_.video_started = true;
				stats_.video_frames++;
			}
			stats_.video_bytes += n;
			host_.video(p, n, ts, config);
			if (!config && cfg_.video_fps > 0) {
				video_frame_in(ts);
				return;
			}
		} else if (ai >= 0) {
			stats_.audio_bytes[ai] += n;
			host_.audio(ch, p, n);
		}
		ack_media(ch);
		return;
	}
	case kMicRequest:                                     // MicrophoneRequest
		pb::get_u(p, n, 1, v);
		log_msg("aa: MICROPHONE_REQUEST %s -> ok (no mic data)", v ? "open" : "close");
		w.u(1, 0);                                        // MicrophoneResponse: status OK
		if (media_session_[ch] >= 0)
			w.u(2, (u64)media_session_[ch]);
		send_msg(ch, aap::kEncrypted, kMicResponse, w.buf);
		return;
	case kVideoFocusRequest:
		pb::get_u(p, n, 2, v);
		pb::get_u(p, n, 3, idx);
		log_msg("aa: VIDEO_FOCUS_REQUEST mode %u reason %u -> projected", (unsigned)v, (unsigned)idx);
		send_video_focus();
		return;
	}
	log_unknown(ch, id, n);
}

// A video frame came: its ack is due one frame period after the last one
// (or now, if the phone was slower than that). Acks are never saved up: a
// phone that paused gets no burst of quick acks afterwards.
void Session::video_frame_in(u64 ts_us)
{
	u64 now = (u64)host_.now_ms() * 1000;
	i64 age = (i64)(now / 1000) - (i64)(ts_us / 1000);
	if (ts_us != 0) {
		if (!age_known_ || age < age_min_) {
			age_min_ = age;
			age_known_ = true;
		}
		stats_.age_ms = (unsigned)(age - age_min_);
		if (stats_.age_ms > stats_.age_ms_max)
			stats_.age_ms_max = stats_.age_ms;
	}
	u64 period = 1000000 / cfg_.video_fps;
	if (acks_due_ == 0) {
		u64 due = next_ack_us_ + period;    // after the last ack sent
		next_ack_us_ = due > now ? due : now;
		held_since_us_ = now;
	}
	acks_due_++;
	send_due_acks();
}

void Session::send_due_acks()
{
	if (acks_due_ == 0)
		return;
	u64 now = (u64)host_.now_ms() * 1000;
	if (now < next_ack_us_)
		return;
	unsigned waited = (unsigned)((now - held_since_us_) / 1000);
	if (waited > 0) {
		stats_.acks_held++;
		stats_.ack_wait_ms_sum += waited;
		if (waited > stats_.ack_wait_ms_max)
			stats_.ack_wait_ms_max = waited;
	}
	ack_media(kChVideo);
	acks_due_--;
	// next_ack_us_ stays the nominal time of this ack, so a late tick
	// doesn't lower the rate. More than one frame without its ack (the
	// phone sent past max unacked): the next one a period later.
	if (acks_due_ > 0) {
		held_since_us_ = now;
		next_ack_us_ += 1000000 / cfg_.video_fps;
	}
}

unsigned Session::ms_to_next(unsigned limit) const
{
	if (acks_due_ == 0)
		return limit;
	u64 now = (u64)host_.now_ms() * 1000;
	if (now >= next_ack_us_)
		return 0;
	u64 ms = (next_ack_us_ - now + 999) / 1000;
	return ms < limit ? (unsigned)ms : limit;
}

void Session::on_sensor(u16 id, const u8 *p, usize n)
{
	if (id != kSensorRequest) {
		log_unknown(kChSensor, id, n);
		return;
	}
	u64 type = 0, period = 0;
	pb::get_u(p, n, 1, type);
	pb::get_u(p, n, 2, period);
	bool ok = type == kSensorDrivingStatus || type == kSensorNightMode;
	log_msg("aa: SENSOR_REQUEST type %u every %u ms: %s", (unsigned)type, (unsigned)period,
	        ok ? "ok" : "not offered");
	pb::Writer w;
	w.i(1, ok ? 0 : -9);                                 // SUCCESS / INVALID_SENSOR
	send_msg(kChSensor, aap::kEncrypted, kSensorResponse, w.buf);
	if (!ok)
		return;
	pb::Writer batch, data;
	if (type == kSensorDrivingStatus) {
		data.i(1, 0);                                    // DRIVE_STATUS_UNRESTRICTED
		batch.msg(13, data);
	} else {
		data.b(1, false);                                // day
		batch.msg(10, data);
	}
	send_msg(kChSensor, aap::kEncrypted, kSensorBatch, batch.buf);
}

void Session::on_input(u16 id, const u8 *, usize n)
{
	if (id != kKeyBindingRequest) {
		log_unknown(kChInput, id, n);
		return;
	}
	log_msg("aa: KEY_BINDING_REQUEST, %zu bytes -> ok", n);
	pb::Writer w;
	w.i(1, 0);
	send_msg(kChInput, aap::kEncrypted, kKeyBindingResponse, w.buf);
}

void Session::tick()
{
	if (phase_ != Phase::Running)
		return;
	send_due_acks();
	unsigned now = host_.now_ms();
	if (now - last_ping_ >= cfg_.ping_ms) {
		last_ping_ = now;
		pb::Writer w;
		w.i(1, now);
		send_msg(kChControl, 0, kPingRequest, w.buf);    // plain, as aasdk
		stats_.pings_sent++;
	}
	if (!pong_warned_ && now - last_pong_ >= kPongWarnMs) {
		pong_warned_ = true;
		log_msg("aa: no ping answer for %u ms (%u sent, %u answered)", now - last_pong_,
		        stats_.pings_sent, stats_.pongs);
	}
}

void Session::bye()
{
	if (phase_ != Phase::Running)
		return;
	pb::Writer w;
	w.u(1, 1);                                           // USER_SELECTION
	send_msg(kChControl, aap::kEncrypted, kByeByeRequest, w.buf);
}

} // namespace aa

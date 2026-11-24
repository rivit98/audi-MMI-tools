// audio_rx.cpp - see audio_rx.h.
#include "audio_rx.h"
#include "sbc.h"

#include <cstdio>

namespace audio {

namespace {

void add(Counts &into, const Counts &c)
{
	into.msgs += c.msgs;
	into.frames += c.frames;
	into.bad += c.bad;
	into.missing += c.missing;
	into.back += c.back;
	into.silent += c.silent;
	into.short_msgs += c.short_msgs;
	into.bytes += c.bytes;
	if (c.gap_max_ms > into.gap_max_ms)
		into.gap_max_ms = c.gap_max_ms;
	if (c.lag_seen) {
		if (!into.lag_seen || c.lag_min < into.lag_min)
			into.lag_min = c.lag_min;
		if (!into.lag_seen || c.lag_max > into.lag_max)
			into.lag_max = c.lag_max;
		into.lag_last = c.lag_last;
		into.lag_seen = true;
	}
}

// Every scale factor 0: the encoder saw silence.
bool silent(const u8 *f, const sbc::Header &h)
{
	usize bits = (h.mode == sbc::kJoint ? h.subbands : 0) + 4u * h.subbands * h.channels;
	usize skip = h.mode == sbc::kJoint ? h.subbands : 0;   // the join bits
	for (usize b = skip; b < bits; b++)
		if ((f[4 + b / 8] >> (7 - b % 8)) & 1)
			return false;
	return true;
}

} // namespace

std::string Rx::config(const u8 *p, unsigned n, unsigned now_ms, std::string *ended)
{
	ended->clear();
	if (on_)
		*ended = end("a new AUDIO_CONFIG", now_ms);
	auto c = proto::parse_audio_config(p, n);
	if (!c)
		return "not an SBC config this receiver takes";
	usize want = sbc::frame_len(c->mode, c->subbands, c->blocks, c->bitpool);
	if (want != c->frame_len) {
		char why[96];
		std::snprintf(why, sizeof(why), "frame_len %u, but the fields make %zu",
		              (unsigned)c->frame_len, want);
		return why;
	}
	cfg_ = *c;
	samples_ = (u32)c->blocks * c->subbands;
	on_ = true;
	next_seq_ = 0;
	t0_ms_ = win_ms_ = now_ms;
	any_msg_ = false;
	win_ = Counts();
	total_ = Counts();
	capture_.clear();
	capture_done_ = capture_taken_ = false;
	return "";
}

void Rx::count_frame(const u8 *f, bool good)
{
	if (!good) {
		win_.bad++;
		return;
	}
	sbc::Header h;
	sbc::parse(f, cfg_.frame_len, h);
	win_.frames++;
	win_.bytes += cfg_.frame_len;
	if (silent(f, h))
		win_.silent++;
	if (!capture_done_ && cap_ > 0) {
		capture_.insert(capture_.end(), f, f + cfg_.frame_len);
		if (capture_.size() + cfg_.frame_len > cap_)
			capture_done_ = true;
	}
}

void Rx::data(const u8 *p, unsigned n, unsigned now_ms)
{
	if (!on_)
		return;
	if (n < proto::kAudioDataHdr) {
		win_.short_msgs++;
		return;
	}
	u32 seq = proto::get_u32(p);
	u32 phone_ms = proto::get_u32(p + 4);
	win_.msgs++;
	if (any_msg_) {
		unsigned gap = now_ms - last_msg_ms_;
		if (gap > win_.gap_max_ms)
			win_.gap_max_ms = gap;
	} else {
		phone0_ = phone_ms;
		mmi0_ = now_ms;
		any_msg_ = true;
	}
	last_msg_ms_ = now_ms;
	// arrival vs capture, both from the first message: + = later than then
	i64 lag = (i64)(i32)(now_ms - mmi0_) - (i64)(i32)(phone_ms - phone0_);
	if (!win_.lag_seen || lag < win_.lag_min)
		win_.lag_min = lag;
	if (!win_.lag_seen || lag > win_.lag_max)
		win_.lag_max = lag;
	win_.lag_last = lag;
	win_.lag_seen = true;

	if (seq > next_seq_)
		win_.missing += seq - next_seq_;
	else if (seq < next_seq_)
		win_.back++;

	const u8 *f = p + proto::kAudioDataHdr;
	unsigned left = n - proto::kAudioDataHdr, count = 0;
	while (left > 0) {
		sbc::Header h;
		bool good = left >= cfg_.frame_len && sbc::frame_ok(f, left, &h) &&
		            h.frame_len == cfg_.frame_len && h.rate == cfg_.rate &&
		            h.blocks == cfg_.blocks && h.subbands == cfg_.subbands &&
		            h.mode == cfg_.mode && h.bitpool == cfg_.bitpool;
		if (left < cfg_.frame_len) {
			win_.bad++;          // a frame cut short: the rest of the message
			break;
		}
		count_frame(f, good);
		f += cfg_.frame_len;
		left -= cfg_.frame_len;
		count++;
	}
	next_seq_ = seq + count;
}

std::string Rx::line(const char *what, const Counts &c, unsigned ms) const
{
	f64 audio_s = cfg_.rate ? (f64)c.frames * samples_ / cfg_.rate : 0;
	f64 s = ms / 1000.0;
	char buf[400];
	std::snprintf(buf, sizeof(buf),
	              "audio: %s %.1f s: %u frames = %.1f s of audio (%.0f%% of real time), %.0f "
	              "kbit/s, %u bad, %u missing, %u back, %u silent, %u msgs (gap max %u ms, %u "
	              "short), lag min/max/last %lld/%lld/%lld ms",
	              what, s, c.frames, audio_s, s > 0 ? 100.0 * audio_s / s : 0.0,
	              s > 0 ? (f64)c.bytes * 8 / 1000 / s : 0.0, c.bad, c.missing, c.back, c.silent,
	              c.msgs, c.gap_max_ms, c.short_msgs, (long long)c.lag_min,
	              (long long)c.lag_max, (long long)c.lag_last);
	return buf;
}

std::string Rx::window(unsigned now_ms, bool force)
{
	if (!on_)
		return "";
	unsigned ms = now_ms - win_ms_;
	if (ms < kWindowMs && !(force && (win_.msgs || win_.short_msgs)))
		return "";
	std::string out = line("", win_, ms);
	add(total_, win_);
	win_ = Counts();
	win_ms_ = now_ms;
	return out;
}

std::string Rx::end(const char *why, unsigned now_ms)
{
	if (!on_)
		return "";
	std::string last = window(now_ms, true);
	char head[120];
	std::snprintf(head, sizeof(head), "stream ended (%s), in all", why);
	std::string out = last.empty() ? "" : last + "\n";
	out += line(head, total_, now_ms - t0_ms_);
	on_ = false;
	capture_done_ = true;
	return out;
}

std::vector<u8> Rx::take_capture()
{
	if (!capture_done_ || capture_taken_ || capture_.empty())
		return {};
	capture_taken_ = true;
	std::vector<u8> out;
	out.swap(capture_);
	return out;
}

} // namespace audio

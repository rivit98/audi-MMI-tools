// hw_stream.cpp - see hw_stream.h.
#include "hw_stream.h"

#include "camera.h"
#include "vout.h"

extern "C" {
#include "log.h"
}

#include <climits>

std::string HwStream::start(const HwOptions &o)
{
	stop();
	opts_ = o;
	playing_ = failed_ = false;
	t_play_ = t_video_ = 0;
	video_on_ = o.video_on_already;
	if (video_on_) {
		t_video_ = log_ms();
		vout::set_mode(o.vcap_mode);
	}
	std::string err = stream_.configure(o.cfg);
	if (!err.empty())
		return "stream: " + err;
	const h264::Sps &s = stream_.sps();
	log_msg("  hw: %ux%u, profile %u level %u, POC type %u, %u fps, %u-byte frames, "
	        "%u declared (%.0f min), header %llu bytes, gap fill %s",
	        s.width, s.height, (unsigned)s.profile, (unsigned)s.level, s.poc_type, o.cfg.fps,
	        stream_.sample_size(), stream_.declared(),
	        stream_.declared() / (double)o.cfg.fps / 60.0,
	        (unsigned long long)stream_.data_off(),
	        !o.cfg.fill ? "off" : stream_.skips_possible() ? "on" : "impossible for this stream");
	err = server_.start(o.path.c_str(), &stream_);
	if (!err.empty())
		return "server: " + err;
	return "";
}

live::Push HwStream::push(const uint8_t *annexb, size_t n, std::string *why)
{
	return stream_.push(annexb, n, log_ms(), why);
}

std::string HwStream::poll(const std::function<void(unsigned, int64_t)> &on_time)
{
	if (!server_.running() || failed_)
		return failed_ ? "failed" : "";
	if (!playing_ && stream_.started()) {
		// the first IDR is in: io-media's first read (64 KB at 0) will
		// soon be answerable
		std::string err = player_.open(opts_.graph, opts_.path);
		if (err.empty())
			err = player_.play();
		if (!err.empty()) {
			failed_ = true;
			player_.close();
			return "player: " + err;
		}
		playing_ = true;
		t_play_ = log_ms();
	}
	// not over the rear camera: the picture comes when it has gone
	if (playing_ && !video_on_ && opts_.show_video && log_ms() - t_play_ >= opts_.video_after_ms &&
	    camera::clear()) {
		vout::show(opts_.vcap_mode);
		video_on_ = true;
		t_video_ = log_ms();
	}
	if (playing_)
		player_.poll(on_time);
	return "";
}

void HwStream::stop(bool keep_video)
{
	if (video_on_ && !keep_video)
		vout::hide();
	video_on_ = false;
	if (server_.running())
		stream_.finish();
	if (playing_) {
		player_.poll({});
		player_.close();
	}
	playing_ = false;
	server_.stop();
}

int64_t HwStream::lag_ms(int64_t pos_ms) const
{
	if (pos_ms < 0)
		return INT64_MIN;
	uint64_t frame = (uint64_t)pos_ms * opts_.cfg.fps / 1000;
	int64_t at = stream_.push_ms(frame);
	if (at < 0)
		return INT64_MIN;
	return (int64_t)log_ms() - at;
}

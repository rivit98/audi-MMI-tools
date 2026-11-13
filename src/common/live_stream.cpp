// live_stream.cpp - see live_stream.h.
#include "live_stream.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace live {

namespace {

bool same(const h264::Nal &nal, const std::vector<uint8_t> &v)
{
	return nal.n == v.size() && std::memcmp(nal.p, v.data(), nal.n) == 0;
}

} // namespace

const char *push_name(Push p)
{
	switch (p) {
	case Push::Ok: return "ok";
	case Push::WaitIdr: return "waiting for an IDR";
	case Push::TooBig: return "too big";
	case Push::Bad: return "bad";
	case Push::ConfigChanged: return "SPS/PPS changed";
	case Push::Full: return "stream full";
	case Push::Shed: return "ahead of the clock";
	case Push::NotConfigured: return "not configured";
	}
	return "?";
}

std::string check_decodable(const h264::Sps &s, unsigned fps)
{
	if (s.profile != 66)
		return "profile " + std::to_string(s.profile) + ", not Baseline (66)";
	if (s.level > 30)
		return "level " + std::to_string(s.level / 10) + "." + std::to_string(s.level % 10) +
		       ", above 3.0";
	if (s.width > 720 || s.height > 576)
		return std::to_string(s.width) + "x" + std::to_string(s.height) +
		       ": io-media takes at most 720x576";
	if (!s.frame_mbs_only)
		return "interlaced (field) coding";
	if (fps == 0 || fps > 30)
		return std::to_string(fps) + " fps: 1..30 only";
	if (fps > 25 && s.height > 480)
		return "more than 25 fps above 480 lines";
	return "";
}

std::string Stream::configure(const Config &c)
{
	std::lock_guard<std::mutex> lock(mu_);

	header_.clear();
	ring_.clear();
	ring_ms_.clear();
	frames_ = 0;
	started_ = need_idr_ = finished_ = sync_ = filling_ = shedding_ = false;
	t0_ms_ = last_push_ms_ = 0;
	prev_ref_frame_num_ = last_poc_lsb_ = 0;
	st_ = Stats();
	declared_ = 0;
	data_off_ = 0;

	cfg_ = c;
	if (cfg_.ring_frames < 8)
		cfg_.ring_frames = 8;
	std::string err = h264::parse_sps(c.sps.data(), c.sps.size(), sps_);
	if (err.empty())
		err = h264::parse_pps(c.pps.data(), c.pps.size(), pps_);
	if (!err.empty())
		return err;
	if (pps_.sps_id != sps_.id)
		return "the PPS refers to another SPS";
	if (c.sample_size < 4096 || c.sample_size > 1024 * 1024)
		return "sample size must be 4 KB..1 MB";
	skip_ok_ = h264::skip_supported(sps_, pps_).empty();

	uint32_t most = csmp4::max_frames(c.sample_size);
	declared_ = c.declared == 0 || c.declared > most ? most : c.declared;
	err = csmp4::build_header(c.sps, c.pps, c.fps, c.sample_size, declared_, header_);
	if (!err.empty()) {
		declared_ = 0;
		return err;
	}
	data_off_ = header_.size();
	ring_.assign((size_t)cfg_.ring_frames * c.sample_size, 0);
	ring_ms_.assign(cfg_.ring_frames, -1);
	csmp4::filler_sample(c.sample_size, filler_);
	return "";
}

void Stream::store(const std::vector<uint8_t> &sample, uint64_t now_ms)
{
	size_t slot = (size_t)(frames_ % cfg_.ring_frames);
	std::memcpy(ring_.data() + slot * cfg_.sample_size, sample.data(), cfg_.sample_size);
	ring_ms_[slot] = (int64_t)now_ms;
	frames_++;
	st_.frames = (uint32_t)frames_;
}

Push Stream::push(const uint8_t *annexb, size_t n, uint64_t now_ms, std::string *why)
{
	std::lock_guard<std::mutex> lock(mu_);
	auto fail = [&](Push p, const std::string &w) {
		if (why != nullptr)
			*why = w;
		return p;
	};

	if (header_.empty())
		return fail(Push::NotConfigured, "no config");
	if (finished_)
		return fail(Push::Full, "the stream has ended");
	if (frames_ >= declared_)
		return fail(Push::Full, "all declared frames used");

	std::vector<h264::Nal> nals = h264::split_annexb(annexb, n);
	const h264::Nal *first = nullptr;
	bool ref = false;
	for (const h264::Nal &nal : nals) {
		uint8_t t = nal.type();
		if ((t == h264::kSps && !same(nal, cfg_.sps)) || (t == h264::kPps && !same(nal, cfg_.pps))) {
			st_.config_changed++;
			return fail(Push::ConfigChanged, t == h264::kSps ? "in-band SPS differs"
			                                                 : "in-band PPS differs");
		}
		if ((t == h264::kSlice || t == h264::kIdr) && first == nullptr)
			first = &nal;
		if ((t == h264::kSlice || t == h264::kIdr) && nal.ref_idc() != 0)
			ref = true;
	}
	if (first == nullptr) {
		st_.bad++;
		return fail(Push::Bad, "no slice in the frame");
	}
	h264::Slice s;
	std::string err = h264::parse_slice(first->p, first->n, sps_, pps_, s);
	if (!err.empty()) {
		st_.bad++;
		return fail(Push::Bad, err);
	}
	// ahead of the frame clock (frame k due k / fps after the first): drop
	// until it is back on it, then go on from an IDR
	if (started_ && cfg_.shed_ahead_ms != 0) {
		int64_t ahead = (int64_t)(frames_ * 1000 / cfg_.fps) - (int64_t)(now_ms - t0_ms_);
		if (!shedding_ && ahead > (int64_t)cfg_.shed_ahead_ms) {
			shedding_ = true;
			st_.sheds++;
		}
		if (shedding_) {
			if (ahead > 0) {
				st_.shed++;
				need_idr_ = true;
				return fail(Push::Shed, std::to_string(ahead) + " ms ahead");
			}
			shedding_ = false;
			sync_ = true;
		}
	}
	if ((!started_ || need_idr_) && !s.idr()) {
		st_.wait_idr++;
		return fail(Push::WaitIdr, started_ ? "after dropped frames" : "before the first");
	}

	std::vector<std::pair<const uint8_t *, size_t>> parts;
	size_t bytes = 0;
	for (const h264::Nal &nal : nals) {
		parts.emplace_back(nal.p, nal.n);
		if (nal.type() != h264::kAud)
			bytes += 4 + nal.n;
	}
	if (bytes > st_.max_frame)
		st_.max_frame = (uint32_t)bytes;
	err = csmp4::make_sample_nals(parts, cfg_.sample_size, sample_);
	if (!err.empty()) {
		st_.too_big++;
		if (ref || s.idr()) {
			need_idr_ = true;
			sync_ = true;
		}
		return fail(Push::TooBig, err);
	}

	if (!started_) {
		started_ = true;
		t0_ms_ = now_ms;
	}
	need_idr_ = false;
	if (started_) {
		int64_t ahead = (int64_t)(frames_ * 1000 / cfg_.fps) - (int64_t)(now_ms - t0_ms_);
		if (ahead > st_.ahead_ms_max)
			st_.ahead_ms_max = ahead;
	}
	if (ref)
		prev_ref_frame_num_ = s.frame_num;
	last_poc_lsb_ = s.poc_lsb;
	store(sample_, now_ms);
	last_push_ms_ = now_ms;
	filling_ = false;
	st_.real++;
	if (s.idr())
		st_.idr++;
	return Push::Ok;
}

unsigned Stream::fill(uint64_t now_ms)
{
	std::lock_guard<std::mutex> lock(mu_);
	unsigned added = 0;

	if (!cfg_.fill || !skip_ok_ || !started_ || need_idr_ || finished_ || header_.empty())
		return 0;
	unsigned idle = cfg_.fill_after_ms ? cfg_.fill_after_ms : 1000 / cfg_.fps;
	if (now_ms < last_push_ms_ + idle)
		return 0;
	// frames due by now on the frame clock (frame k at k / fps after the
	// first); fill up to one short of it, never past
	uint64_t due = (now_ms - t0_ms_) * cfg_.fps / 1000 + 1;
	while (frames_ + 1 < due && frames_ < declared_) {
		unsigned max_fn = 1u << sps_.log2_max_frame_num;
		unsigned max_poc = 1u << sps_.log2_max_poc_lsb;
		last_poc_lsb_ = (last_poc_lsb_ + 1) % max_poc;
		std::vector<uint8_t> nal =
			h264::skip_slice(sps_, pps_, (prev_ref_frame_num_ + 1) % max_fn, last_poc_lsb_);
		std::vector<std::pair<const uint8_t *, size_t>> parts{{nal.data(), nal.size()}};
		if (!csmp4::make_sample_nals(parts, cfg_.sample_size, sample_).empty())
			break;
		if (!filling_) {
			filling_ = true;
			st_.gaps++;
		}
		store(sample_, now_ms);
		last_push_ms_ = now_ms;
		st_.skips++;
		added++;
	}
	return added;
}

void Stream::finish()
{
	std::lock_guard<std::mutex> lock(mu_);
	finished_ = true;
}

bool Stream::finished() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return finished_;
}

bool Stream::sync_wanted()
{
	std::lock_guard<std::mutex> lock(mu_);
	bool s = sync_;
	sync_ = false;
	return s;
}

uint64_t Stream::available() const
{
	std::lock_guard<std::mutex> lock(mu_);
	if (header_.empty())
		return 0;
	return data_off_ + frames_ * cfg_.sample_size;
}

uint64_t Stream::frames() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return frames_;
}

size_t Stream::read(uint64_t off, uint8_t *dst, size_t n, ReadInfo *info)
{
	std::lock_guard<std::mutex> lock(mu_);
	const uint64_t end_all = data_off_ + (uint64_t)declared_ * cfg_.sample_size;

	if (header_.empty() || off >= end_all)
		return 0;
	if (off + n > end_all)
		n = (size_t)(end_all - off);
	size_t done = 0;
	while (done < n) {
		uint64_t at = off + done;
		size_t len;
		if (at < data_off_) {
			len = std::min<size_t>(n - done, (size_t)(data_off_ - at));
			std::memcpy(dst + done, header_.data() + at, len);
		} else {
			uint64_t k = (at - data_off_) / cfg_.sample_size;
			uint32_t j = (uint32_t)((at - data_off_) % cfg_.sample_size);
			len = std::min<size_t>(n - done, cfg_.sample_size - j);
			const uint8_t *src;
			if (k >= frames_) {
				src = filler_.data();                 // far read
				if (info != nullptr)
					info->ahead++;
			} else if (k + cfg_.ring_frames < frames_) {
				src = filler_.data();                 // no longer kept
				st_.evicted_reads++;
				if (info != nullptr)
					info->evicted++;
			} else {
				src = ring_.data() + (size_t)(k % cfg_.ring_frames) * cfg_.sample_size;
			}
			std::memcpy(dst + done, src + j, len);
		}
		done += len;
	}
	return n;
}

int64_t Stream::push_ms(uint64_t k) const
{
	std::lock_guard<std::mutex> lock(mu_);
	if (k >= frames_ || k + cfg_.ring_frames < frames_ || ring_ms_.empty())
		return -1;
	return ring_ms_[(size_t)(k % cfg_.ring_frames)];
}

bool Stream::started() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return started_;
}

uint64_t Stream::start_ms() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return t0_ms_;
}

Stats Stream::stats() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return st_;
}

} // namespace live

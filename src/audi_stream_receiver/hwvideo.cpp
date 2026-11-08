// hwvideo.cpp - see hwvideo.h. The stream itself (CS-MP4, resource
// manager, io-media graph, video displayable) is common/hw_stream.h, the
// code hello9 runs on the car with a clip instead of the phone.
#include "hwvideo.h"

#include "config_gate.h"
#include "hw_stream.h"
#include "live_stream.h"
#include "vout.h"

extern "C" {
#include "log.h"
#include "proto.h"
}

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <climits>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>
#include <unistd.h>

namespace {

constexpr const char *kPath = "/dev/audi_stream/live.mp4";
constexpr unsigned kTickMs = 20;
constexpr unsigned kWindowMs = 10000;   // an "h264:" summary line per window
constexpr unsigned kSyncEveryMs = 500;  // at most one SYNC per this
constexpr unsigned kLogRefusals = 5;    // refused frames logged one by one
constexpr uint32_t kRingBytes = 8u << 20; // frames kept for io-media: 8 MB (>= 16 frames)

struct Window {
	uint32_t real = 0, skips = 0, refused = 0;
	unsigned reads = 0, waited = 0;
	std::vector<int64_t> lags;
};

std::mutex mu;                          // everything below, but `hw` use by the hw thread
std::unique_ptr<HwStream> hw;           // set / taken under mu; polled by the hw thread
ConfigGate gate;                        // the newest config: running, being set up or pending
std::vector<uint8_t> config_next;       // a new one for the hw thread
bool config_pending = false;
bool stop_pending = false;
std::string stop_why;
bool restart_pending = false;           // stream full: same config again
std::deque<uint8_t> statuses;
unsigned last_sync_ms = 0;
uint32_t refused_total = 0, refused_logged = 0;
void (*notify)(void) = nullptr;

std::atomic<bool> active{false};
std::atomic<bool> quit{false};
pthread_t thread;
bool thread_started = false;
unsigned graphs = 0;
unsigned kept_since = 0;                // hw thread: screen kept for a stream that may follow
bool video_held = false;                // hw thread: the picture left on for the next stream
constexpr unsigned kKeepScreenMs = 5000;

// How a stream ends. /dev/videoctrl (VCAP 0) is shared with the HMI's rear
// camera and nothing says whether the camera is on (docs/rear-camera.md §9,
// §11): an app that sets VP1, or disables it, while the camera is up hides
// the camera from the driver. So the picture is switched only when the
// user starts / stops streaming - never by a restart, never after an error.
enum class End {
	Stop,        // the user's end (VIDEO_STOP, a picture, unplugged, app exit): picture off, VCAP restored
	Restart,     // another stream follows at once: picture and VCAP stay for it
	Error,       // io-media failed (the HMI may have taken VCAP 0): our displayable goes, VCAP untouched
};

// mu held
void queue_status(uint8_t code)
{
	if (code == PROTO_H264_SYNC) {
		if (log_ms() - last_sync_ms < kSyncEveryMs && last_sync_ms != 0)
			return;
		last_sync_ms = log_ms();
	}
	if (statuses.size() < 16)
		statuses.push_back(code);
}

void notify_sender()
{
	if (notify != nullptr)
		notify();
}

const char *status_name(uint8_t c)
{
	switch (c) {
	case PROTO_H264_PLAYING: return "PLAYING";
	case PROTO_H264_SYNC: return "SYNC";
	case PROTO_H264_REFUSED: return "REFUSED";
	case PROTO_H264_CONFIG_WANTED: return "CONFIG";
	}
	return "?";
}

void status(uint8_t code)
{
	{
		std::lock_guard<std::mutex> lock(mu);
		queue_status(code);
	}
	notify_sender();
}

// hw thread: give up a picture left on for a stream that didn't come.
void release_held_video(bool restore_vcap)
{
	if (!video_held)
		return;
	vout::hide(restore_vcap);
	video_held = false;
}

// hw thread: the config taken didn't give a stream, and no other is waiting.
void forget_config()
{
	std::lock_guard<std::mutex> lock(mu);
	if (!config_pending)
		gate.forget();
}

// hw thread: end the stream (if any) and log what it did.
void teardown(const char *why, End how = End::Stop)
{
	std::unique_ptr<HwStream> h;
	{
		std::lock_guard<std::mutex> lock(mu);
		h.swap(hw);                      // no more frames into it
	}
	if (how == End::Error)
		forget_config();
	if (!h) {
		if (how != End::Restart)
			release_held_video(how == End::Stop);
		return;
	}
	bool had_video = h->video_on();
	h->stop(how != End::Stop);
	if (how == End::Restart)
		video_held = had_video;
	else if (how == End::Error && had_video)
		vout::hide(false);
	live::Stats ls = h->stream().stats();
	StreamStats ss = h->server().stats();
	const PlayStats &ps = h->player().stats();
	log_msg("h264: stream ended (%s): %u frames in (%u IDR), %u skips in %u gaps, refused: %u "
	        "too big, %u waiting for an IDR, %u bad, %u SPS/PPS changed; io-media: %u reads, %u "
	        "waited (max %u ms), %u unblocked; %u TIME events, position %.1f s, %u errors%s%s",
	        why, ls.real, ls.idr, ls.skips, ls.gaps, ls.too_big, ls.wait_idr, ls.bad,
	        ls.config_changed, ss.reads, ss.waited, ss.wait_ms_max, ss.unblocked, ps.time_events,
	        ps.max_pos_ms < 0 ? 0.0 : ps.max_pos_ms / 1000.0, ps.error_events,
	        ps.error_events ? ": " : "", ps.first_error.c_str());
	if (how != End::Restart)
		active = false;
	else
		kept_since = log_ms();
}

// hw thread: a stream from an H264_CONFIG payload.
void setup(const std::vector<uint8_t> &payload)
{
	struct proto_h264_config c;
	if (proto_h264_config_parse(payload.data(), (unsigned)payload.size(), &c) != 0) {
		log_msg("h264: H264_CONFIG malformed (%zu bytes): refused", payload.size());
		forget_config();
		release_held_video(true);
		active = false;
		status(PROTO_H264_REFUSED);
		return;
	}
	HwOptions o;
	o.cfg.sps.assign(c.sps, c.sps + c.sps_len);
	o.cfg.pps.assign(c.pps, c.pps + c.pps_len);
	o.cfg.fps = c.fps;
	uint32_t s = std::max<uint32_t>(c.frame_bytes + csmp4::kFillerMin, 8192);
	o.cfg.sample_size = (s + 4095) / 4096 * 4096;
	o.cfg.declared = 0;                  // as many as fit: ~1.6 h at 28 KB x 25 fps
	o.cfg.fill = true;
	o.cfg.ring_frames = std::min<uint32_t>(128, std::max<uint32_t>(16, kRingBytes / o.cfg.sample_size));
	o.path = kPath;
	char g[48];
	std::snprintf(g, sizeof(g), "audi_stream_%d_%u", (int)getpid(), ++graphs);
	o.graph = g;
	o.video_on_already = video_held;

	h264::Sps sps;
	std::string err = h264::parse_sps(o.cfg.sps.data(), o.cfg.sps.size(), sps);
	if (err.empty())
		err = live::check_decodable(sps, c.fps);
	if (err.empty() && c.frame_bytes > HWVIDEO_MAX_FRAME)
		err = "frame_bytes " + std::to_string(c.frame_bytes) + " over " +
		      std::to_string(HWVIDEO_MAX_FRAME);
	log_msg("h264: config: %u fps, frames up to %u bytes (padded to %u), SPS %u / PPS %u "
	        "bytes, %ux%u profile %u level %u POC type %u%s%s",
	        c.fps, (unsigned)c.frame_bytes, (unsigned)o.cfg.sample_size, c.sps_len, c.pps_len,
	        sps.width, sps.height, (unsigned)sps.profile, (unsigned)sps.level, sps.poc_type,
	        err.empty() ? "" : ": refused, ", err.c_str());
	if (!err.empty()) {
		forget_config();
		release_held_video(true);
		active = false;
		status(PROTO_H264_REFUSED);
		return;
	}

	std::unique_ptr<HwStream> h(new HwStream());
	active = true;                       // the main loop closes its screen
	err = h->start(o);
	video_held = false;                  // h owns the picture now
	if (!err.empty()) {
		log_msg("h264: %s: refused", err.c_str());
		forget_config();
		h->stop();
		active = false;
		status(PROTO_H264_REFUSED);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mu);
		hw.swap(h);
		refused_logged = 0;
		queue_status(PROTO_H264_PLAYING);
		last_sync_ms = 0;
		queue_status(PROTO_H264_SYNC);   // start at an IDR
	}
	notify_sender();
	log_msg("h264: streaming at %s, graph %s; waiting for the first IDR", kPath, o.graph.c_str());
}

void log_window(HwStream &h, Window &w, unsigned ms)
{
	live::Stats ls = h.stream().stats();
	StreamStats ss = h.server().stats();
	std::vector<int64_t> lags;
	lags.swap(w.lags);
	std::sort(lags.begin(), lags.end());
	uint32_t refused;
	{
		std::lock_guard<std::mutex> lock(mu);
		refused = refused_total;
	}
	log_msg("h264: %.1f s: %u frames (%.1f fps), %u skips, %u refused; io-media %u reads, %u "
	        "waited (max %u ms), lead max %lld frames; lag %lld/%lld/%lld ms (%zu TIME), "
	        "position %.1f s%s",
	        ms / 1000.0, ls.real - w.real, (ls.real - w.real) * 1000.0 / (ms ? ms : 1),
	        ls.skips - w.skips, refused - w.refused, ss.reads - w.reads, ss.waited - w.waited,
	        ss.wait_ms_max, ss.lead_frames_max == INT64_MIN ? 0LL : (long long)ss.lead_frames_max,
	        lags.empty() ? 0LL : (long long)lags.front(),
	        lags.empty() ? 0LL : (long long)lags[lags.size() / 2],
	        lags.empty() ? 0LL : (long long)lags.back(), lags.size(),
	        h.player().stats().max_pos_ms < 0 ? 0.0 : h.player().stats().max_pos_ms / 1000.0,
	        h.video_on() ? "" : ", picture not on yet");
	w.real = ls.real;
	w.skips = ls.skips;
	w.refused = refused;
	w.reads = ss.reads;
	w.waited = ss.waited;
}

void *run(void *)
{
	Window w;
	unsigned window_t0 = 0;
	bool was_playing = false;

	while (!quit) {
		std::vector<uint8_t> next;
		bool have_next = false, do_stop = false, do_restart = false;
		std::string why;
		{
			std::lock_guard<std::mutex> lock(mu);
			if (stop_pending) {
				do_stop = true;
				why = stop_why;
				stop_pending = false;
			}
			if (config_pending) {
				next.swap(config_next);
				have_next = true;
				config_pending = false;
			}
			if (restart_pending && !have_next) {
				next = gate.current();
				do_restart = !next.empty();
			}
			restart_pending = false;
		}
		if (do_stop)
			teardown(why.c_str());
		if (have_next || do_restart) {
			teardown(have_next ? "new config" : "all declared frames used: starting again", End::Restart);
			setup(next);
			w = Window();
			window_t0 = log_ms();
			was_playing = false;
		}

		HwStream *h = hw.get();          // only this thread changes hw
		if (h == nullptr && active && log_ms() - kept_since > kKeepScreenMs) {
			log_msg("h264: no new stream after %u s: the screen goes back", kKeepScreenMs / 1000);
			release_held_video(true);
			active = false;
		}
		if (h != nullptr) {
			std::string err = h->poll([&w, h](unsigned, int64_t pos) {
				int64_t lag = h->lag_ms(pos);
				if (lag != INT64_MIN && w.lags.size() < 2000)
					w.lags.push_back(lag);
			});
			if (!err.empty()) {
				log_msg("h264: %s", err.c_str());
				teardown("the graph would not play", End::Error);
				status(PROTO_H264_REFUSED);
				continue;
			}
			const PlayStats &ps = h->player().stats();
			if (ps.error_events > 0) {
				// io-media gave up on the stream: maybe the HMI took the
				// video (its media, the rear camera). Not restarted, and
				// VCAP left as it is (End::Error); the phone goes back to
				// pictures for this connection.
				log_msg("h264: io-media error: %s", ps.first_error.c_str());
				teardown("io-media reported an error", End::Error);
				status(PROTO_H264_REFUSED);
				continue;
			}
			if (h->playing() && !was_playing) {
				was_playing = true;
				log_msg("h264: first IDR in, graph playing");
			}
			if (h->stream().sync_wanted()) {
				log_msg("h264: a reference frame was dropped: asking for an IDR");
				status(PROTO_H264_SYNC);
			}
			for (const std::string &n : h->server().take_notes())
				if (n.find("unblocked") != std::string::npos || n.find("far read") != std::string::npos ||
				    n.find("opened") != std::string::npos)
					log_msg("h264: server: %s", n.c_str());
			if (log_ms() - window_t0 >= kWindowMs) {
				log_window(*h, w, log_ms() - window_t0);
				window_t0 = log_ms();
			}
		}
		usleep(kTickMs * 1000);
	}
	teardown("the app is stopping");
	return nullptr;
}

} // namespace

extern "C" {

void hwvideo_start(void)
{
	if (thread_started)
		return;
	quit = false;
	thread_started = pthread_create(&thread, nullptr, run, nullptr) == 0;
	if (!thread_started)
		log_msg("h264: no thread: hardware video off");
}

void hwvideo_shutdown(void)
{
	if (!thread_started)
		return;
	quit = true;
	pthread_join(thread, nullptr);
	thread_started = false;
}

void hwvideo_set_notify(void (*fn)(void))
{
	notify = fn;
}

void hwvideo_config(const uint8_t *payload, unsigned n)
{
	std::vector<uint8_t> p(payload, payload + n);
	{
		std::lock_guard<std::mutex> lock(mu);
		if (gate.offer(p)) {
			config_next.swap(p);
			config_pending = true;
			return;
		}
		if (!hw || config_pending)
			return;                  // on its way: setup() says PLAYING
		queue_status(PROTO_H264_PLAYING);
	}
	log_msg("h264: H264_CONFIG again, the same: nothing to do");
	notify_sender();
}

int hwvideo_frame(const uint8_t *data, unsigned size)
{
	bool wake = false;
	int ack;
	std::string note;                    // logged after the lock: log.c fsyncs
	{
		std::lock_guard<std::mutex> lock(mu);
		if (!hw) {
			refused_total++;
			if (refused_logged++ < kLogRefusals)
				note = gate.taken() ? "the stream is starting" : "before any H264_CONFIG";
			if (gate.taken()) {
				// setup() asks for an IDR when it is done
				ack = PROTO_VIDEO_DROPPED;
			} else {
				if (gate.ask(log_ms())) {
					queue_status(PROTO_H264_CONFIG_WANTED);
					wake = true;
				}
				ack = PROTO_VIDEO_BAD;
			}
		} else {
			std::string why;
			live::Push p = hw->push(data, size, &why);
			ack = PROTO_VIDEO_SHOWN;
			if (p != live::Push::Ok) {
				refused_total++;
				if (refused_logged++ < kLogRefusals)
					note = std::string(live::push_name(p)) + (why.empty() ? "" : ", ") + why;
				ack = p == live::Push::WaitIdr ? PROTO_VIDEO_DROPPED : PROTO_VIDEO_BAD;
				if (p == live::Push::WaitIdr) {
					queue_status(PROTO_H264_SYNC);
					wake = true;
				} else if (p == live::Push::ConfigChanged) {
					if (gate.ask(log_ms())) {
						queue_status(PROTO_H264_CONFIG_WANTED);
						wake = true;
					}
				} else if (p == live::Push::Full) {
					restart_pending = true;
				}
			}
		}
	}
	if (!note.empty())
		log_msg("h264: frame (%u bytes) refused: %s", size, note.c_str());
	if (wake)
		notify_sender();
	return ack;
}

void hwvideo_stop(const char *why)
{
	std::lock_guard<std::mutex> lock(mu);
	if (!hw && !gate.taken())
		return;
	stop_pending = true;
	stop_why = why;
	config_pending = false;
	gate.forget();
}

int hwvideo_status_pop(uint8_t *code)
{
	{
		std::lock_guard<std::mutex> lock(mu);
		if (statuses.empty())
			return 0;
		*code = statuses.front();
		statuses.pop_front();
	}
	log_msg("h264: tx H264_STATUS %s", status_name(*code));
	return 1;
}

int hwvideo_active(void)
{
	return active ? 1 : 0;
}

} // extern "C"

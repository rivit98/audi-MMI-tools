// aa_host - android_auto's desk test: this PC plays the head unit for a
// real phone over libusb, with the same session code the MMI runs
// (session.cpp, tls.cpp, frame.h, pb.h). `make aa-host` runs it in the
// mmi-aa-host container (USB + X11 passed through).
//
//   1. any USB device that answers AOA GET_PROTOCOL is switched to accessory
//      mode with the Android Auto strings; the phone comes back as
//      18d1:2d0x
//   2. the session runs until the phone goes or Ctrl-C (bye-bye first)
//   3. the phone's video goes to <out>/android_auto.h264, the audio
//      channels to <out>/media.pcm (48 kHz stereo), guidance.pcm and
//      system.pcm (16 kHz mono), all s16le; with --play the video is also
//      shown live by ffplay
//
// In a container libusb keeps the device list it found at start-up, so
// when the bus changes (the phone re-enumerating) the process starts over
// (execv), like tools/aoa_host.py.
//
// usage: aa_host [--out DIR] [--cert DIR] [--play] [--no-audio] [--version M.m] [--tls N] [--fps N]
//   --fps N  ack video frames at most N per second (default 25, 0 = at once)
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

extern "C" {
#include "log.h"
}

#include "mono_cond.h"
#include "session.h"
#include "usb_chunk.h"

namespace {

constexpr uint16_t kAoaVid = 0x18D1;
constexpr uint16_t kAoaPidMin = 0x2D00, kAoaPidMax = 0x2D05;
const char *const kStrings[6] = {"Android", "Android Auto", "Android Auto", "2.0.1",
                                 "https://github.com/dspl1236/MMI3G-Toolkit", "MMI3G-AA-DESK"};
constexpr unsigned kInBuf = 16384;
constexpr unsigned kOutMax = 16384;

std::atomic<bool> quit{false};

void on_signal(int)
{
	quit = true;
}

std::set<std::string> bus_devices()
{
	std::set<std::string> s;
	DIR *d = opendir("/sys/bus/usb/devices");
	if (d == nullptr)
		return s;
	while (dirent *e = readdir(d))
		if (e->d_name[0] != '.' && std::strchr(e->d_name, ':') == nullptr) {
			// the name is the port path, the same when a phone re-enumerates
			// on its port (as the accessory): the device number is not
			char path[300], num[16] = "";
			std::snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/devnum", e->d_name);
			if (FILE *f = std::fopen(path, "r")) {
				if (std::fgets(num, sizeof(num), f) == nullptr)
					num[0] = 0;
				std::fclose(f);
			}
			s.insert(std::string(e->d_name) + "#" + num);
		}
	closedir(d);
	return s;
}

std::string slurp(const std::string &path)
{
	std::string s;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return s;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	std::fclose(f);
	return s;
}

// AOA handshake on one device: true if START went out.
bool try_switch(libusb_device *dev, const libusb_device_descriptor &dd)
{
	libusb_device_handle *h;
	if (libusb_open(dev, &h) != 0)
		return false;
	uint8_t ver[2] = {};
	int r = libusb_control_transfer(h, 0xC0, 51, 0, 0, ver, 2, 1000);
	bool ok = r == 2 && (ver[0] | ver[1] << 8) >= 1;
	if (ok) {
		log_msg("%04x:%04x speaks AOA v%d, switching to Android Auto", dd.idVendor, dd.idProduct,
		        ver[0] | ver[1] << 8);
		for (int i = 0; i < 6 && ok; i++)
			ok = libusb_control_transfer(h, 0x40, 52, 0, (uint16_t)i, (unsigned char *)kStrings[i],
			                             (uint16_t)(std::strlen(kStrings[i]) + 1), 1000) >= 0;
		ok = ok && libusb_control_transfer(h, 0x40, 53, 0, 0, nullptr, 0, 1000) >= 0;
		if (!ok)
			log_msg("  AOA strings / START failed");
	}
	libusb_close(h);
	return ok;
}

struct Accessory {
	libusb_device_handle *h = nullptr;
	int                   iface = -1;
	uint8_t               in = 0, out = 0;
	unsigned              out_packet = 512;
};

bool open_accessory(libusb_device *dev, Accessory &a)
{
	libusb_config_descriptor *cfg;
	if (libusb_open(dev, &a.h) != 0 || libusb_get_active_config_descriptor(dev, &cfg) != 0)
		return false;
	for (int i = 0; i < cfg->bNumInterfaces && a.iface < 0; i++) {
		const libusb_interface_descriptor &id = cfg->interface[i].altsetting[0];
		if (id.bInterfaceClass != 0xff || id.bInterfaceSubClass != 0xff)
			continue;
		for (int e = 0; e < id.bNumEndpoints; e++) {
			const libusb_endpoint_descriptor &ep = id.endpoint[e];
			if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
				continue;
			if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN)
				a.in = ep.bEndpointAddress;
			else {
				a.out = ep.bEndpointAddress;
				a.out_packet = ep.wMaxPacketSize;
			}
		}
		a.iface = id.bInterfaceNumber;
	}
	libusb_free_config_descriptor(cfg);
	if (a.iface < 0 || a.in == 0 || a.out == 0)
		return false;
	libusb_set_auto_detach_kernel_driver(a.h, 1);
	return libusb_claim_interface(a.h, a.iface) == 0;
}

class Desk : public aa::Host {
public:
	Desk(Accessory &a, const std::string &out, bool play) : a_(a)
	{
		video_ = std::fopen((out + "/android_auto.h264").c_str(), "wb");
		const char *names[3] = {"media.pcm", "guidance.pcm", "system.pcm"};
		for (int i = 0; i < 3; i++)
			audio_[i] = std::fopen((out + "/" + names[i]).c_str(), "wb");
		if (play) {
			player_ = popen("ffplay -loglevel warning -window_title 'Android Auto (desk)' "
			                "-fflags nobuffer -flags low_delay -framedrop -f h264 -", "w");
			if (player_ == nullptr)
				log_msg("cannot start ffplay");
		}
	}
	~Desk()
	{
		for (FILE *f : {video_, audio_[0], audio_[1], audio_[2]})
			if (f)
				std::fclose(f);
		if (player_)
			pclose(player_);
	}

	void send(const uint8_t *p, size_t n) override
	{
		while (n > 0 && !failed) {
			size_t c = usb_chunk(n, kOutMax, a_.out_packet);
			int got = 0;
			int r = libusb_bulk_transfer(a_.h, a_.out, const_cast<uint8_t *>(p), (int)c, &got, 2000);
			if (r != 0) {
				log_msg("bulk OUT: %s", libusb_error_name(r));
				failed = true;
				return;
			}
			p += got;
			n -= (size_t)got;
		}
	}
	void video(const uint8_t *p, size_t n, uint64_t, bool) override
	{
		if (video_)
			std::fwrite(p, 1, n, video_);
		if (player_ && std::fwrite(p, 1, n, player_) == n)
			std::fflush(player_);
	}
	void audio(uint8_t ch, const uint8_t *p, size_t n) override
	{
		int i = ch - aa::kChMediaAudio;
		if (i >= 0 && i < 3 && audio_[i])
			std::fwrite(p, 1, n, audio_[i]);
	}
	unsigned now_ms() override { return log_ms(); }

	std::atomic<bool> failed{false};

private:
	Accessory &a_;
	FILE      *video_ = nullptr, *audio_[3] = {}, *player_ = nullptr;
};

void run_session(Accessory &a, const aa::Config &cfg, const std::string &cert, const std::string &key,
                 const std::string &out, bool play)
{
	Desk desk(a, out, play);
	aa::Session s(cfg, desk);
	std::mutex lock;
	MonoCond cond;
	std::deque<std::vector<uint8_t>> rxq;

	std::thread reader([&] {
		std::vector<uint8_t> buf(kInBuf);
		while (!quit && !desk.failed) {
			int got = 0;
			int r = libusb_bulk_transfer(a.h, a.in, buf.data(), (int)buf.size(), &got, 200);
			if (r == LIBUSB_ERROR_TIMEOUT && got == 0)
				continue;
			if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
				log_msg("bulk IN: %s", libusb_error_name(r));
				desk.failed = true;
				break;
			}
			std::lock_guard<std::mutex> g(lock);
			rxq.emplace_back(buf.begin(), buf.begin() + got);
			cond.notify();
		}
		cond.notify();
	});

	s.start(cert, key);
	unsigned last_stats = log_ms();
	while (!quit && !desk.failed && s.phase() != aa::Phase::Failed && s.phase() != aa::Phase::Ended) {
		std::deque<std::vector<uint8_t>> q;
		{
			std::unique_lock<std::mutex> g(lock);
			cond.wait_ms(g, s.ms_to_next(50), [&] { return !rxq.empty() || desk.failed || quit; });
			q.swap(rxq);
		}
		for (const auto &b : q)
			s.rx(b.data(), b.size());
		s.tick();
		if (log_ms() - last_stats >= 10000) {
			last_stats = log_ms();
			const aa::Stats &st = s.stats();
			log_msg("stats: video %llu frames %llu KB, audio %llu/%llu/%llu KB, rtt %u ms, pings %u/%u; "
			        "pace %u fps: %llu acks held (max %u ms), frame age +%u ms (max +%u ms)",
			        (unsigned long long)st.video_frames, (unsigned long long)(st.video_bytes >> 10),
			        (unsigned long long)(st.audio_bytes[0] >> 10), (unsigned long long)(st.audio_bytes[1] >> 10),
			        (unsigned long long)(st.audio_bytes[2] >> 10), st.last_rtt_ms, st.pongs, st.pings_sent,
			        cfg.video_fps, (unsigned long long)st.acks_held, st.ack_wait_ms_max, st.age_ms,
			        st.age_ms_max);
		}
	}
	if (quit && s.phase() == aa::Phase::Running) {
		s.bye();
		usleep(300 * 1000);
	}
	desk.failed = true;
	reader.join();
	log_msg("session over: %s%s%s", aa::phase_name(s.phase()), s.stats().error.empty() ? "" : ": ",
	        s.stats().error.c_str());
}

} // namespace

int main(int argc, char **argv)
{
	std::string out = ".", cert_dir = "third_party/aasdk-cert";
	bool play = false;
	aa::Config cfg;
	int tls = 1;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		unsigned m, n;
		if (a == "--out" && i + 1 < argc)
			out = argv[++i];
		else if (a == "--cert" && i + 1 < argc)
			cert_dir = argv[++i];
		else if (a == "--play")
			play = true;
		else if (a == "--no-audio")
			cfg.audio = false;
		else if (a == "--version" && i + 1 < argc && std::sscanf(argv[++i], "%u.%u", &m, &n) == 2) {
			cfg.version_major = (uint16_t)m;
			cfg.version_minor = (uint16_t)n;
		} else if (a == "--tls" && i + 1 < argc)
			tls = std::atoi(argv[++i]);
		else if (a == "--fps" && i + 1 < argc)
			cfg.video_fps = (unsigned)std::atoi(argv[++i]);
		else {
			std::fprintf(stderr, "usage: %s [--out DIR] [--cert DIR] [--play] [--no-audio] "
			                     "[--version M.m] [--tls N] [--fps N]\n", argv[0]);
			return 2;
		}
	}
	log_open((out + "/android_auto.log").c_str());
	std::string cert = slurp(cert_dir + "/headunit.crt"), key = slurp(cert_dir + "/headunit.key");
	if (cert.empty() || key.empty()) {
		log_msg("no head unit certificate in %s (make fetches it)", cert_dir.c_str());
		return 1;
	}
	aa::Tls::set_debug(tls);
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::signal(SIGPIPE, SIG_IGN);

	const std::set<std::string> bus_at_start = bus_devices();
	libusb_context *ctx;
	if (libusb_init(&ctx) != 0) {
		log_msg("libusb_init failed");
		return 1;
	}
	std::set<std::pair<int, int>> tried;
	log_msg("waiting for a phone (plug it in; Ctrl-C to stop)");
	while (!quit) {
		if (bus_devices() != bus_at_start) {
			log_msg("USB devices changed: restarting to see them");
			libusb_exit(ctx);
			execv("/proc/self/exe", argv);
			return 1;
		}
		libusb_device **list;
		ssize_t n = libusb_get_device_list(ctx, &list);
		libusb_device *acc = nullptr;
		for (ssize_t i = 0; i < n && acc == nullptr; i++) {
			libusb_device_descriptor dd;
			if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.bDeviceClass == LIBUSB_CLASS_HUB)
				continue;
			if (dd.idVendor == kAoaVid && dd.idProduct >= kAoaPidMin && dd.idProduct <= kAoaPidMax) {
				acc = libusb_ref_device(list[i]);
				break;
			}
			auto k = std::make_pair((int)libusb_get_bus_number(list[i]), (int)libusb_get_device_address(list[i]));
			if (tried.insert(k).second)
				try_switch(list[i], dd);
		}
		libusb_free_device_list(list, 1);
		if (acc != nullptr) {
			Accessory a;
			if (open_accessory(acc, a)) {
				log_msg("accessory open: IN 0x%02x OUT 0x%02x (%u B packets)", a.in, a.out, a.out_packet);
				run_session(a, cfg, cert, key, out, play);
				libusb_release_interface(a.h, a.iface);
			} else {
				log_msg("accessory: no ff/ff interface with bulk IN + OUT, or claim failed");
			}
			if (a.h)
				libusb_close(a.h);
			libusb_unref_device(acc);
			break;                        // one session per run
		}
		sleep(1);
	}
	libusb_exit(ctx);
	log_close();
	return 0;
}

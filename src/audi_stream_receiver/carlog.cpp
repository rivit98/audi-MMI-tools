// carlog.cpp - see carlog.h.
#include "carlog.h"
#include "types.h"

#include "camera.h"
#include "carsig.h"
#include "ipc4.h"

extern "C" {
#include "cpumon.h"
#include "log.h"
}

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

namespace carlog {

namespace {

constexpr unsigned kTickMs = 1000;
constexpr unsigned kWindowMs = 60000;
// The decoded file stops growing here (a few MB an hour while driving).
constexpr usize kMaxTxtBytes = 64u << 20;

std::mutex mu;                          // the state below, between the threads
carsig::Logger logger;
std::vector<std::string> lines_q;       // -> log_msg
std::vector<std::string> txt_q;         // -> the ch4 file
u32 odd = 0;                            // telegrams not shaped like MOST

int sub = -1;
std::thread thr;
std::atomic<bool> quit{false};
FILE *txt = nullptr;
usize txt_bytes = 0;

// On the camera library's reader thread: short, no SD writes.
void on_ch4(u32 ms, const u8 *, usize, const std::vector<ipc4::Telegram> &ts)
{
	std::lock_guard<std::mutex> lock(mu);
	for (const ipc4::Telegram &t : ts) {
		char line[400];
		ipc4::Most m;
		if (t.type != 1 || !ipc4::parse_most(t.body, m)) {
			odd++;
			std::snprintf(line, sizeof(line), "[%7u ms] #%02x type %u, %u bytes: %s", ms,
			              t.counter, t.type, (unsigned)t.body.size(), ipc4::hex(t.body).c_str());
			txt_q.push_back(line);
			continue;
		}
		std::snprintf(line, sizeof(line), "[%7u ms] #%02x %s: %s", ms, t.counter,
		              ipc4::most_key(m).c_str(), ipc4::hex(m.payload).c_str());
		txt_q.push_back(line);
		logger.feed(ms, m, lines_q);
	}
}

void write_out(std::vector<std::string> &ls, std::vector<std::string> &tx)
{
	for (const std::string &l : ls)
		log_msg("car: %s", l.c_str());
	if (txt == nullptr || tx.empty())
		return;
	for (const std::string &l : tx) {
		if (txt_bytes >= kMaxTxtBytes)
			break;
		std::fprintf(txt, "%s\n", l.c_str());
		txt_bytes += l.size() + 1;
	}
	std::fflush(txt);
}

void run()
{
	cpumon_name_thread("carlog");
	unsigned window_start = log_ms();
	while (!quit) {
		usleep(kTickMs * 1000);
		unsigned now = log_ms();
		std::vector<std::string> ls, tx;
		std::string win;
		u32 odd_now = 0;
		{
			std::lock_guard<std::mutex> lock(mu);
			logger.tick(now, lines_q);
			ls.swap(lines_q);
			tx.swap(txt_q);
			if (now - window_start >= kWindowMs) {
				win = logger.window();
				odd_now = odd;
			}
		}
		write_out(ls, tx);
		if (!win.empty()) {
			camera::Ch4Stats c4 = camera::ch4_stats();
			log_msg("car: %u.%u s: %s | ch4 %s, %u telegrams, %u gaps (%u missing), %u not MOST",
			        (now - window_start) / 1000, (now - window_start) % 1000 / 100, win.c_str(),
			        c4.state, c4.telegrams, c4.gaps, c4.missing, odd_now);
			window_start = now;
		}
	}
}

} // namespace

void start(const char *sd_path)
{
	char path[256], prev[256];
	std::snprintf(path, sizeof(path), "%s/audi_stream_receiver_ch4.txt", var_dir(sd_path));
	std::snprintf(prev, sizeof(prev), "%s/audi_stream_receiver_ch4.prev.txt", var_dir(sd_path));
	if (access(path, F_OK) == 0) {
		unlink(prev);
		rename(path, prev);
	}
	txt = std::fopen(path, "w");
	if (txt == nullptr)
		log_msg("car: cannot open %s: the log lines only", path);
	quit = false;
	try {
		thr = std::thread(run);
	} catch (const std::system_error &) {
		log_msg("car: no thread: no car signals");
		if (txt != nullptr)
			std::fclose(txt);
		txt = nullptr;
		return;
	}
	sub = camera::subscribe_ch4(on_ch4, "carlog");
	camera::Ch4Stats c4 = camera::ch4_stats();
	log_msg("car: logging ch4's car signals (ch4 %s); night unknown until DA01.0C0D changes",
	        c4.state);
}

void stop()
{
	if (sub < 0)
		return;
	camera::unsubscribe_ch4(sub);
	sub = -1;
	quit = true;
	thr.join();
	std::vector<std::string> ls, tx;
	std::string win;
	{
		std::lock_guard<std::mutex> lock(mu);
		logger.flush(log_ms(), lines_q);
		ls.swap(lines_q);
		tx.swap(txt_q);
		win = logger.window();
	}
	write_out(ls, tx);
	log_msg("car: end: %s", win.c_str());
	if (txt != nullptr)
		std::fclose(txt);
	txt = nullptr;
}

} // namespace carlog

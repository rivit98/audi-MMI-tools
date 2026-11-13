// player.cpp - see player.h. Event layout as hello5 found it on the car
// (src/hello5/main.c, native_event): {u32 size, u32 type} then the payload;
// TIME = u64 track, s64 position ms, u64 total ms; STATUS = u64 track, u32
// state (4 = playing), s32 speed.
#include "player.h"

extern "C" {
#include "iom.h"
#include "log.h"
}

#include <cerrno>
#include <cstdio>
#include <cstring>

struct Player::iom_state {
	struct iom g;
};

namespace {

// testapp-file -o candidates that attached in hello6's runs, from the
// unit's MME outputdevices table; TDM2 (a rear zone) first. The clips are
// silent or have no audio at all.
const char *const kOutputs[] = {
	"mixer:DSPOUT_TDM2_20?delay=130",
	"mixer:DSPOUT_TDM3_20?delay=130",
	"mixer:DSPOUT_TDM1_51_SAD?delay=130",
};

constexpr int kTimeNotifyMs = 250;

uint32_t rd32(const unsigned char *p)
{
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

uint64_t rd64(const unsigned char *p)
{
	uint64_t v;
	std::memcpy(&v, p, 8);
	return v;
}

} // namespace

std::string Player::fail(const char *what, int rc)
{
	struct iom_err e;
	char line[200];

	if (g_ != nullptr && iom_error_info(&g_->g, &e) == EOK) {
		std::snprintf(line, sizeof(line), "%s: %s, iom_errno=0x%x type=%u value=%llu", what,
		              std::strerror(rc), (unsigned)e.iom_errno, (unsigned)e.value_type,
		              (unsigned long long)e.value);
		if (events_n_ > 0) {
			PlayEvent &last = events_[(events_n_ - 1) % kEventHistory];
			if (last.type == IOM_EV_ERROR && last.a == -1) {
				last.a = (int64_t)e.iom_errno;
				last.b = (int64_t)e.value;
			}
		}
	} else
		std::snprintf(line, sizeof(line), "%s: %s", what, std::strerror(rc));
	log_msg("  player: %s", line);
	return line;
}

std::string Player::open(const std::string &name, const std::string &url)
{
	close();
	st_ = PlayStats();
	name_ = name;
	g_ = new iom_state();
	int rc = iom_create(&g_->g, name.c_str());
	if (rc != EOK) {
		std::string e = "create graph " + name + ": " + std::strerror(rc);
		delete g_;
		g_ = nullptr;
		return e;
	}
	if ((rc = iom_set_speed(&g_->g, 1000)) != EOK)
		fail("set speed", rc);
	if ((rc = iom_set_time_notify(&g_->g, kTimeNotifyMs)) != EOK)
		fail("set time notification", rc);
	output_ = nullptr;
	for (const char *o : kOutputs) {
		rc = iom_attach_output(&g_->g, o);
		if (rc == EOK) {
			output_ = o;
			break;
		}
		fail(o, rc);
	}
	if (output_ == nullptr)
		return "no output would attach";
	rc = iom_attach_input(&g_->g, 1, url.c_str());
	if (rc != EOK)
		return fail("attach input", rc);
	log_msg("  player: graph %s, output %s (id %d), input %s", g_->g.path, output_,
	        g_->g.output, url.c_str());
	return "";
}

std::string Player::play()
{
	if (g_ == nullptr)
		return "no graph";
	t_play_ = log_ms();
	int rc = iom_play(&g_->g);
	return rc == EOK ? "" : fail("play", rc);
}

void Player::remember(uint32_t type, int64_t a, int64_t b)
{
	PlayEvent &e = events_[events_n_++ % kEventHistory];
	e.at_ms = log_ms();
	e.type = type;
	e.a = a;
	e.b = b;
}

std::vector<PlayEvent> Player::recent_events() const
{
	std::vector<PlayEvent> out;
	unsigned n = events_n_ < kEventHistory ? events_n_ : kEventHistory;
	for (unsigned i = events_n_ - n; i != events_n_; i++)
		out.push_back(events_[i % kEventHistory]);
	return out;
}

std::string Player::describe(const PlayEvent &e)
{
	char line[96];

	switch (e.type) {
	case IOM_EV_TIME:
		std::snprintf(line, sizeof(line), "TIME %.1f s of %.1f s", e.a / 1000.0, e.b / 1000.0);
		break;
	case IOM_EV_STATUS:
		std::snprintf(line, sizeof(line), "STATUS state %lld speed %lld", (long long)e.a,
		              (long long)e.b);
		break;
	case IOM_EV_INPUT_EOF:
		std::snprintf(line, sizeof(line), "INPUT_EOF");
		break;
	case IOM_EV_FINISHED:
		std::snprintf(line, sizeof(line), "FINISHED");
		break;
	case IOM_EV_ERROR:
		std::snprintf(line, sizeof(line), "ERROR iom_errno=0x%llx value=%llu",
		              (unsigned long long)e.a, (unsigned long long)e.b);
		break;
	default:
		std::snprintf(line, sizeof(line), "type %u (%lld bytes)", (unsigned)e.type,
		              (long long)e.a);
		break;
	}
	return line;
}

void Player::event(uint32_t type, const unsigned char *p, uint32_t len,
                   const std::function<void(unsigned, int64_t)> &on_time)
{
	const unsigned t = log_ms() - t_play_;

	switch (type) {
	case IOM_EV_STATUS:
		remember(type, len >= 16 ? (int64_t)rd32(p + 8) : -1, len >= 16 ? (int32_t)rd32(p + 12) : 0);
		break;
	case IOM_EV_TIME:
		remember(type, len >= 24 ? (int64_t)rd64(p + 8) : -1, len >= 24 ? (int64_t)rd64(p + 16) : 0);
		break;
	case IOM_EV_ERROR:
		remember(type, -1, 0);           // fail() below fills in io-media's error
		break;
	default:
		remember(type, (int64_t)len + 8, 0);
		break;
	}

	switch (type) {
	case IOM_EV_STATUS:
		if (len < 16)
			break;
		log_msg("  +%u ms STATUS state %u speed %d", t, (unsigned)rd32(p + 8), (int)rd32(p + 12));
		if (rd32(p + 8) == 4 && st_.playing_ms == 0)
			st_.playing_ms = t ? t : 1;
		return;
	case IOM_EV_TIME: {
		if (len < 24)
			break;
		int64_t pos = (int64_t)rd64(p + 8);
		if (st_.time_events++ == 0) {
			st_.first_time_ms = t ? t : 1;
			log_msg("  +%u ms first TIME: at %lld of %llu ms", t, (long long)pos,
			        (unsigned long long)rd64(p + 16));
		}
		if (pos > st_.max_pos_ms)
			st_.max_pos_ms = pos;
		if (on_time)
			on_time(t, pos);
		return;
	}
	case IOM_EV_INPUT_EOF:
		st_.input_eof = true;
		log_msg("  +%u ms INPUT_EOF", t);
		return;
	case IOM_EV_FINISHED:
		st_.finished = true;
		log_msg("  +%u ms FINISHED", t);
		return;
	case IOM_EV_ERROR: {
		std::string e = fail("ERROR event", EIO);
		if (st_.error_events++ == 0)
			st_.first_error = e;
		return;
	}
	}
	log_msg("  +%u ms event type %u, %u bytes", t, (unsigned)type, (unsigned)len + 8);
}

void Player::poll(const std::function<void(unsigned, int64_t)> &on_time)
{
	unsigned char buf[512];

	if (g_ == nullptr || blind_)
		return;
	for (;;) {
		int len = iom_read_events(&g_->g, buf, sizeof(buf));
		if (len < 0) {
			log_msg("  player: reading events: %s; going on without them", std::strerror(errno));
			blind_ = true;
			return;
		}
		if (len == 0)
			return;
		for (int off = 0; off + 8 <= len;) {
			uint32_t size = rd32(buf + off);
			if (size < 8 || off + size > (uint32_t)len) {
				log_msg("  player: bad event size %u at %d of %d bytes", (unsigned)size, off, len);
				break;
			}
			event(rd32(buf + off + 4), buf + off + 8, size - 8, on_time);
			off += (int)size;
		}
	}
}

void Player::close()
{
	if (g_ == nullptr)
		return;
	iom_close(&g_->g);
	delete g_;
	g_ = nullptr;
}

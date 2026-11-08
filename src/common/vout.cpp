// vout.cpp - see vout.h. The devctl codes and payloads are dev-videoctrl's
// (io_devctl at 0x08042640, P1001), as in hello6's video.c:
//   {int vp, int value}, 8 bytes: SET_SOURCE / ENABLE store the value for
//   the VP; UPDATE (4 bytes, 0) applies every pending value and slogs
//   "VP1: source changed to 1" / "VP1: vcap enabled".
// VP 1 = VCAP 0, whose input 1 is the DM642's video port.
#include "vout.h"

extern "C" {
#include "lmgr.h"
#include "log.h"
}

#include <cerrno>
#include <cstring>

#include <devctl.h>
#include <fcntl.h>
#include <unistd.h>

namespace vout {

namespace {

constexpr int kCid = 41;
constexpr int kWidth = 800, kHeight = 480;
constexpr int kFlags = 0x101;           // LMGR_VIDEO | VCAP 0
constexpr int kVp = 1, kSource = 1;

constexpr unsigned kSetSource = 0x80080501u;
constexpr unsigned kUpdate = 0x80040507u;
constexpr unsigned kEnable = 0x8008050bu;
constexpr unsigned kSetMode = 0x8008050cu;   // value 0 BOB, 1 WEAVE, 2 ODD ONLY

int vc_fd = -1;
bool lm_is_open = false;
Mode mode_now = Mode::Keep;             // what this process last asked for

int vc_set(unsigned dcmd, int value)
{
	int data[2] = {kVp, value};
	return devctl(vc_fd, (int)dcmd, data, sizeof(data), nullptr);
}

int vc_update()
{
	int zero = 0;
	return devctl(vc_fd, (int)kUpdate, &zero, sizeof(zero), nullptr);
}

bool vc_open()
{
	if (vc_fd < 0)
		vc_fd = open("/dev/videoctrl", O_RDWR);
	if (vc_fd < 0) {
		log_msg("  /dev/videoctrl: %s", std::strerror(errno));
		return false;
	}
	return true;
}

int capture(bool on)
{
	if (!vc_open())
		return -1;
	int rc_src = vc_set(kSetSource, on ? kSource : 0);
	int rc_en = vc_set(kEnable, on ? 1 : 0);
	int rc_up = vc_update();
	log_msg("  videoctrl VP %d: source %d rc=%d, %s rc=%d, update rc=%d", kVp, on ? kSource : 0,
	        rc_src, on ? "enable" : "disable", rc_en, rc_up);
	return rc_src ? rc_src : rc_en ? rc_en : rc_up;
}

} // namespace

const char *mode_name(Mode m)
{
	switch (m) {
	case Mode::Bob: return "BOB";
	case Mode::Weave: return "WEAVE";
	case Mode::OddOnly: return "ODD ONLY";
	default: return "unchanged";
	}
}

int set_mode(Mode m)
{
	if (m == mode_now)
		return 0;
	if (!vc_open())
		return -1;
	Mode to = m == Mode::Keep ? Mode::Weave : m;
	int rc_mode = vc_set(kSetMode, to == Mode::Bob ? 0 : to == Mode::Weave ? 1 : 2);
	int rc_up = vc_update();
	log_msg("  videoctrl VP %d: mode %s rc=%d, update rc=%d", kVp, mode_name(to), rc_mode, rc_up);
	mode_now = m;
	return rc_mode ? rc_mode : rc_up;
}

int show(Mode mode)
{
	int rc = lm_open();
	if (rc != 0) {
		log_msg("  lm_open: %s", std::strerror(rc));
		return rc;
	}
	lm_is_open = true;
	lm_unregister_displayable(kCid);
	int ok = 0, cid = kCid;
	rc = lm_register_displayable(kCid, kWidth, kHeight, kFlags, &ok);
	log_msg("  register video displayable %d, flags 0x%03x: rc=%d, result=%d", kCid, kFlags, rc,
	        ok);
	if (rc != 0 || !ok)
		return rc ? rc : -1;
	rc = lm_show_displayables(0, &cid, 1);
	log_msg("  lm_show_displayables(0, {%d}): rc=%d", kCid, rc);
	if (rc != 0)
		return rc;
	usleep(500 * 1000);                     // the layer manager binds the VCAP
	int rc_mode = set_mode(mode);
	rc = capture(true);
	return rc ? rc : rc_mode;
}

void hide(bool restore_vcap)
{
	set_mode(Mode::Keep);
	if (restore_vcap)
		capture(false);
	else
		log_msg("  videoctrl left alone (VCAP 0 may be the HMI's now)");
	if (!lm_is_open && lm_open() != 0)
		return;
	log_msg("  unregister video displayable %d: rc=%d", kCid, lm_unregister_displayable(kCid));
	lm_close();
	lm_is_open = false;
}

void close_all()
{
	if (vc_fd >= 0)
		close(vc_fd);
	vc_fd = -1;
}

} // namespace vout

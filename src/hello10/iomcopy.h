// iomcopy.h - a second io-media-nvidia, run from RAM next to the HMI's own
// (docs/phone-mirroring.md 7.14): io-media's capture width can't be patched
// in the running process (/proc write refused), so a copy of the binary,
// optionally with the width words changed, is started with the original's
// config plus its own mountpoint. The original keeps /dev/io-media and runs
// on; the copy is stopped at the end.
//
// What io-media needs and what is shared (P1001 binaries):
//  - path: the config's global-options mountpoint replaces /dev/io-media
//  - foreground: -B, else it calls procmgr_daemon and the pid changes
//  - the DSP: io-media is a client of devg-NVTS (/dev/nvdsprm0) and
//    devg-NVMiniRM (/dev/nvminirm0); the DSP image is loaded once at boot
//    by NvElfLoader, so the copy doesn't load it again
//  - environment: srv-starter's for io-media (NV_* below)
// Not known: whether devg-NVTS takes a second client, and what the copy's
// start-up does to the DSP tasks the original runs (audio mixer, health
// monitor). The test finds out; start() logs what appeared in /dev, the name
// space and shared memory.
#ifndef HELLO10_IOMCOPY_H
#define HELLO10_IOMCOPY_H

#include "types.h"

#include <string>

#include <sys/types.h>

namespace iomcopy {

constexpr const char *kMount = "/dev/hello10-iom";
constexpr const char *kBinary = "/mnt/efs-system/sbin/io-media-nvidia";
constexpr const char *kConfig = "/etc/iom.cfg";

class Copy {
public:
	~Copy() { stop(); }
	// bin_dir: where to put the binary and config (RAM, executable);
	// out_dir: logs and listings. width: the capture width to patch in
	// (720 = unchanged). Returns "" once kMount is up, else the error (the
	// copy is stopped again).
	std::string start(const std::string &bin_dir, const std::string &out_dir, u16 width);
	// TERM, then KILL; logs how it ended and whether kMount went away.
	void stop();
	bool running() const { return pid_ > 0; }
	// The copy is still alive (not exited).
	bool alive();
	pid_t pid() const { return pid_; }

private:
	pid_t pid_ = -1;
	std::string name_;
};

// Is pid still there (kill 0)?
bool pid_alive(int pid);

} // namespace iomcopy

#endif

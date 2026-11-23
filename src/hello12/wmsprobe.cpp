// wmsprobe.cpp - see wmsprobe.h.
#include "wmsprobe.h"
#include "types.h"

extern "C" {
#include "log.h"
}

#include <cerrno>
#include <cstring>
#include <string>

#include <devctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *kDir = "/dev/wms";
constexpr const char *kPlayer = "/dev/wms/player1";
constexpr unsigned kConnected = 0x4004051e;   // int: non-zero = "bt connected"
constexpr unsigned kStreamInfo = 0x40100520;  // {state, codec, rate, ?}

const char *codec_name(i32 c)
{
	static const char *const kNames[] = {"PCM", "SBC", "MPG", "AAC", "ATC"};
	return c >= 0 && c < 5 ? kNames[c] : "?";
}

} // namespace

int wms_probe_main()
{
	log_msg("wms: listing %s", kDir);
	DIR *dir = opendir(kDir);
	if (dir == nullptr) {
		log_msg("wms: opendir %s: %s", kDir, std::strerror(errno));
	} else {
		while (struct dirent *e = readdir(dir)) {
			std::string p = std::string(kDir) + "/" + e->d_name;
			struct stat st;
			if (stat(p.c_str(), &st) == 0)
				log_msg("wms:   %s  mode 0%o  size %lld", e->d_name, (unsigned)st.st_mode,
				        (long long)st.st_size);
			else
				log_msg("wms:   %s  (stat: %s)", e->d_name, std::strerror(errno));
		}
		closedir(dir);
	}

	log_msg("wms: open %s read-only", kPlayer);
	int fd = open(kPlayer, O_RDONLY);
	if (fd < 0) {
		log_msg("wms: open: %s", std::strerror(errno));
		return 1;
	}
	i32 connected = -1;
	int rc = devctl(fd, (int)kConnected, &connected, sizeof(connected), nullptr);
	log_msg("wms: devctl 0x%08x (connected?) -> %s, value %d", kConnected,
	        rc == EOK ? "EOK" : std::strerror(rc), (int)connected);
	i32 info[4] = {-1, -1, -1, -1};
	rc = devctl(fd, (int)kStreamInfo, info, sizeof(info), nullptr);
	log_msg("wms: devctl 0x%08x (stream info) -> %s, state %d, codec %d (%s), rate %d (%s), "
	        "word 3 %d",
	        kStreamInfo, rc == EOK ? "EOK" : std::strerror(rc), (int)info[0], (int)info[1],
	        codec_name(info[1]), (int)info[2],
	        info[2] == 0 ? "44100" : info[2] == 1 ? "48000" : "?", (int)info[3]);
	close(fd);
	log_msg("wms: closed");
	return 0;
}

// iomcopy.cpp - see iomcopy.h.
#include "iomcopy.h"

extern "C" {
#include "log.h"
#include "proc.h"
}

#include "iomcopy_cfg.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace iomcopy {
namespace {

constexpr unsigned kUpMs = 15000;   // start -> mountpoint
// srv-starter's environment for io-media-nvidia (process 21)
const char *const kEnv[] = {"NV_ENABLE_DYNAMIC_PRIORITIES=1", "NV_DEMUX=6", "NV_MEDIAVIDEO=1",
                            "NV_AVSYNC=7", "NV_MEDIAAUDIO=6"};
// listed before and after the start: what the copy adds
constexpr const char *kListing =
    "for d in /dev /dev/name/local /dev/name/global /dev/shmem /dev/tsshmem; do "
    "echo \"== $d\"; ls -1 $d 2>&1; done";

bool read_all(const std::string &path, std::vector<u8> &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	out.clear();
	u8 buf[65536];
	usize n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.insert(out.end(), buf, buf + n);
	bool ok = !std::ferror(f);
	std::fclose(f);
	return ok;
}

bool write_all(const std::string &path, const void *data, usize n, mode_t mode)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = std::fwrite(data, 1, n, f) == n;
	ok = std::fclose(f) == 0 && ok;
	return ok && chmod(path.c_str(), mode) == 0;
}

std::string text_of(const std::string &path)
{
	std::vector<u8> v;
	read_all(path, v);
	return std::string(v.begin(), v.end());
}

bool exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

} // namespace

bool pid_alive(int pid)
{
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

std::string Copy::start(const std::string &bin_dir, const std::string &out_dir, u16 width)
{
	stop();
	name_ = width == 720 ? "iom-copy" : "iom-copy" + std::to_string(width);
	if (exists(kMount))
		return std::string(kMount) + " is already there (a copy left behind?)";

	std::vector<u8> bin;
	if (!read_all(kBinary, bin))
		return std::string("read ") + kBinary + ": " + std::strerror(errno);
	if (width != 720) {
		std::string err = patch_width(bin, 720, width);
		if (!err.empty())
			return std::string(kBinary) + ": " + err;
	}
	std::string exe = bin_dir + "/" + name_;
	if (!write_all(exe, bin.data(), bin.size(), 0755))
		return "write " + exe + ": " + std::strerror(errno);

	std::string cfg_in = text_of(kConfig), cfg;
	if (cfg_in.empty())
		return std::string("read ") + kConfig + ": empty or missing";
	std::string err = with_mountpoint(cfg_in, kMount, &cfg);
	if (!err.empty())
		return std::string(kConfig) + ": " + err;
	std::string cfg_path = bin_dir + "/" + name_ + ".cfg";
	if (!write_all(cfg_path, cfg.data(), cfg.size(), 0644))
		return "write " + cfg_path + ": " + std::strerror(errno);
	log_msg("  %s: %zu bytes%s, config %s (mountpoint %s)", exe.c_str(), bin.size(),
	        width != 720 ? ", capture width patched" : ", unchanged", cfg_path.c_str(), kMount);

	std::string before = out_dir + "/" + name_ + "_ls_before.txt";
	std::string after = out_dir + "/" + name_ + "_ls_after.txt";
	std::remove(before.c_str());
	std::remove(after.c_str());
	proc_shell(before.c_str(), 10, kListing);

	// our environment with srv-starter's NV_* settings for io-media on top
	std::vector<std::string> env;
	for (char **e = environ; e != nullptr && *e != nullptr; e++) {
		bool replaced = false;
		for (const char *k : kEnv) {
			const char *eq = std::strchr(k, '=');
			if (std::strncmp(*e, k, (usize)(eq - k) + 1) == 0)
				replaced = true;
		}
		if (!replaced)
			env.push_back(*e);
	}
	for (const char *k : kEnv)
		env.push_back(k);
	std::vector<char *> envp;
	for (std::string &s : env)
		envp.push_back(&s[0]);
	envp.push_back(nullptr);
	char *argv[] = {&exe[0], (char *)"-c", &cfg_path[0], (char *)"-B", nullptr};

	std::string out = out_dir + "/" + name_ + "_out.txt";
	unsigned t0 = log_ms();
	pid_ = proc_spawn(out.c_str(), argv, envp.data());
	if (pid_ < 0) {
		pid_ = -1;
		return "spawn " + exe + ": " + std::strerror(errno);
	}
	log_msg("  %s started, pid %d: %s -c %s -B (+ %zu NV_* variables), output %s", name_.c_str(),
	        (int)pid_, exe.c_str(), cfg_path.c_str(), sizeof(kEnv) / sizeof(kEnv[0]),
	        out.c_str());

	bool up = false;
	while (log_ms() - t0 < kUpMs) {
		if (exists(kMount)) {
			up = true;
			break;
		}
		if (!alive())
			break;
		usleep(100 * 1000);
	}
	unsigned ms = log_ms() - t0;
	proc_shell(after.c_str(), 10, kListing);
	std::vector<std::string> added = added_lines(text_of(before), text_of(after));
	std::string list;
	for (usize i = 0; i < added.size() && i < 40; i++)
		list += (i ? ", " : "") + added[i];
	log_msg("  %s: %s after %u ms; new in /dev, /dev/name, shmem: %s", name_.c_str(),
	        up ? "mountpoint up" : running() ? "NO mountpoint" : "EXITED", ms,
	        list.empty() ? "nothing" : list.c_str());
	if (!up) {
		stop();
		return std::string(kMount) + " never appeared (" + name_ + "_out.txt, the slog)";
	}
	return "";
}

bool Copy::alive()
{
	if (pid_ <= 0)
		return false;
	int st = 0;
	pid_t r = waitpid(pid_, &st, WNOHANG);
	if (r == pid_) {
		log_msg("  %s (pid %d) exited by itself: %s", name_.c_str(), (int)pid_,
		        WIFEXITED(st)   ? ("exit " + std::to_string(WEXITSTATUS(st))).c_str()
		        : WIFSIGNALED(st) ? ("signal " + std::to_string(WTERMSIG(st))).c_str()
		                          : "?");
		pid_ = -1;
		return false;
	}
	return true;
}

void Copy::stop()
{
	if (pid_ <= 0)
		return;
	if (!alive())
		return;
	unsigned t0 = log_ms();
	int st = proc_stop(pid_);
	if (st == -1) {
		kill(-pid_, SIGKILL);   // its process group
		log_msg("  %s (pid %d) still there after TERM + KILL: left behind", name_.c_str(),
		        (int)pid_);
	} else {
		log_msg("  %s (pid %d) stopped in %u ms: %s; %s %s", name_.c_str(), (int)pid_,
		        log_ms() - t0, proc_status_str(st), kMount, exists(kMount) ? "still there" : "gone");
	}
	pid_ = -1;
}

} // namespace iomcopy

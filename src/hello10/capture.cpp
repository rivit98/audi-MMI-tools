/*
 * capture.cpp - see capture.h.
 *
 * Addresses are io-media-nvidia's (ET_EXEC, text segment 0x08040000-
 * 0x082a845c, so runtime = file address). Each word is 0x02d0 (720) in the
 * unit's firmware (io-media-nvidia, rev 51). The loads are mov.w @(disp,pc),
 * whose target is pc + 4 + 2 * disp.
 */
extern "C" {
#include "log.h"
#include "proc.h"
}

#include "capture.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace capture {
namespace {

struct Word {
	u32 addr;            // the constant-pool word (capture width)
	const char *what;
};

// The three pool words (phone-mirroring 7.14, step 5). Each holds 720; the
// heights 480 and 576 sit in the words after them.
const Word kWords[] = {
	{0x0818a190, "NTSC capture, routine A (also read at 0x0818a13c)"},
	{0x0818a26e, "capture, routine B (also read at 0x0818a1d0, 0x0818a1f4)"},
	{0x0818ad20, "capture, routine C (also read at 0x0818ab70)"},
};

// The mov.w instructions that read the words, for the probe's check.
const u32 kLoads[] = {0x0818a13c, 0x0818a180, 0x0818a1d0, 0x0818a1f4, 0x0818ab70};

// Each word we changed, with the value it had.
struct Changed {
	u32 addr;
	u16 old;
};
Changed g_changed[sizeof(kWords) / sizeof(kWords[0])];
int g_nchanged = 0;
int g_fd = -1;
int g_pid = -1;

std::string errstr(const char *what)
{
	return std::string(what) + ": " + std::strerror(errno);
}

// Open /proc/<pid>/as for reading and writing.
std::string open_as(int pid)
{
	if (g_fd >= 0 && g_pid == pid)
		return "";
	if (g_fd >= 0)
		close(g_fd);
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/%d/as", pid);
	g_fd = open(path, O_RDWR);
	if (g_fd < 0)
		return errstr(path);
	g_pid = pid;
	return "";
}

std::string read_u16(int fd, u32 addr, u16 &out)
{
	u8 b[2];
	if (pread(fd, b, 2, (off_t)addr) != 2)
		return errstr("read");
	out = (u16)(b[0] | (b[1] << 8));          // SH-4 is little endian
	return "";
}

std::string write_u16(int fd, u32 addr, u16 v)
{
	u8 b[2] = {(u8)(v & 0xff), (u8)(v >> 8)};
	if (pwrite(fd, b, 2, (off_t)addr) != 2)
		return errstr("write");
	return "";
}

// mov.w @(disp,pc),Rn is 0x9ddd: target = pc + 4 + 2 * disp.
u32 mov_w_target(u32 pc, u16 insn)
{
	return pc + 4 + 2u * (u32)(insn & 0xff);
}

} // namespace

std::string find_io_media(const char *out_path, int &pid)
{
	pid = -1;
	if (proc_shell(out_path, 5, "pidin | grep io-media-nvidia") < 0)
		return "pidin could not start";
	FILE *f = std::fopen(out_path, "r");
	if (f == nullptr)
		return errstr(out_path);
	char line[256];
	std::string found;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (std::strncmp(line, "###", 3) == 0)
			continue;                          // proc_shell's own header
		if (std::strstr(line, "io-media-nvidia") == nullptr)
			continue;
		int p = -1;
		if (std::sscanf(line, " %d", &p) == 1 && p > 0 && pid < 0) {
			pid = p;
			found = line;
		}
	}
	std::fclose(f);
	if (pid < 0)
		return "no io-media-nvidia in pidin";
	log_msg("capture: io-media-nvidia pid %d (pidin: %s)", pid, found.c_str());
	return "";
}

std::string probe(int pid)
{
	std::string err = open_as(pid);
	if (!err.empty())
		return err;
	int bad = 0;
	for (const Word &w : kWords) {
		u16 v[3];
		for (int i = 0; i < 3; i++) {
			err = read_u16(g_fd, w.addr + 2u * (u32)i, v[i]);
			if (!err.empty())
				return err;
		}
		log_msg("probe: 0x%08x = %u, next %u, %u  %s", w.addr, v[0], v[1], v[2], w.what);
		if (v[0] != kWidthFrom)
			bad++;
	}
	for (u32 pc : kLoads) {
		u16 insn = 0;
		err = read_u16(g_fd, pc, insn);
		if (!err.empty())
			return err;
		u32 target = mov_w_target(pc, insn);
		bool is_word = false;
		for (const Word &w : kWords)
			is_word = is_word || target == w.addr;
		log_msg("probe: load 0x%08x insn 0x%04x -> 0x%08x %s", pc, insn, target,
		        is_word ? "(a width word)" : "(NOT a width word)");
		if (!is_word)
			bad++;
	}
	log_msg("probe: %s", bad == 0 ? "all three words read 720 and every load points at one"
	                              : "MISMATCH, see above; apply() would refuse");
	return bad == 0 ? "" : "probe mismatch";
}

std::string apply(int pid)
{
	if (g_nchanged != 0)
		return "already applied";
	std::string err = open_as(pid);
	if (!err.empty())
		return err;
	// Check every word first, so a mismatch changes nothing.
	u16 old[sizeof(kWords) / sizeof(kWords[0])];
	for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++) {
		err = read_u16(g_fd, kWords[i].addr, old[i]);
		if (!err.empty())
			return err;
		if (old[i] != kWidthFrom)
			return "word at " + std::to_string(kWords[i].addr) + " is not 720 (" +
			       std::to_string(old[i]) + "); not patched";
	}
	for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++) {
		err = write_u16(g_fd, kWords[i].addr, kWidthTo);
		if (!err.empty()) {
			log_msg("capture: write at 0x%08x failed: %s", kWords[i].addr, err.c_str());
			restore();               // undo the ones already written
			return err;
		}
		g_changed[g_nchanged++] = {kWords[i].addr, old[i]};
		u16 back = 0;
		err = read_u16(g_fd, kWords[i].addr, back);
		log_msg("capture: 0x%08x 720 -> 800, read back %u%s", kWords[i].addr, back,
		        back == kWidthTo ? "" : " (MISMATCH)");
		if (!err.empty() || back != kWidthTo) {
			restore();
			return err.empty() ? "read back differs" : err;
		}
	}
	return "";
}

void restore()
{
	if (g_fd < 0)
		return;
	for (int i = g_nchanged - 1; i >= 0; i--) {
		std::string err = write_u16(g_fd, g_changed[i].addr, g_changed[i].old);
		log_msg("capture: restore 0x%08x -> %u: %s", g_changed[i].addr, g_changed[i].old,
		        err.empty() ? "ok" : err.c_str());
	}
	g_nchanged = 0;
}

bool applied()
{
	return g_nchanged != 0;
}

} // namespace capture

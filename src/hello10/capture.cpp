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
#include "types.h"
#include "proc.h"
}

#include "capture.h"
#include "mapinfo.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <devctl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <unistd.h>
#include <vector>

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

constexpr u64 kTextVaddr = 0x08040000;   // io-media's text: file offset 0 is here
constexpr u64 kPage = 4096;

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
	for (usize i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++) {
		err = read_u16(g_fd, kWords[i].addr, old[i]);
		if (!err.empty())
			return err;
		if (old[i] != kWidthFrom)
			return "word at " + std::to_string(kWords[i].addr) + " is not 720 (" +
			       std::to_string(old[i]) + "); not patched";
	}
	for (usize i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++) {
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

namespace {

constexpr int kMaxSegs = 512;

// DCMD_PROC_MAPINFO of /proc/<pid>/as (pid 0 = ourselves) into segs.
std::string read_maps(int pid, std::vector<mapinfo::Seg> &segs)
{
	static procfs_mapinfo raw[kMaxSegs];
	char path[64];
	if (pid > 0)
		std::snprintf(path, sizeof(path), "/proc/%d/as", pid);
	else
		std::snprintf(path, sizeof(path), "/proc/self/as");
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return errstr(path);
	int n = 0;
	int rc = devctl(fd, DCMD_PROC_MAPINFO, raw, sizeof(raw), &n);
	close(fd);
	if (rc != EOK)
		return std::string(path) + ": MAPINFO: " + std::strerror(rc);
	if (n > kMaxSegs)
		n = kMaxSegs;
	segs.clear();
	for (int i = 0; i < n; i++) {
		mapinfo::Seg s;
		s.vaddr = raw[i].vaddr;
		s.size = raw[i].size;
		s.flags = raw[i].flags;
		s.dev = (u32)raw[i].dev;
		s.offset = (u64)raw[i].offset;
		s.ino = (u64)raw[i].ino;
		segs.push_back(s);
	}
	return "";
}

void log_seg(const char *who, u64 addr, const mapinfo::Seg *s)
{
	if (s == nullptr) {
		log_msg("maps: %s: no segment holds 0x%08llx", who, (unsigned long long)addr);
		return;
	}
	log_msg("maps: %s 0x%08llx in 0x%08llx +%llu: flags 0x%08x = %s, dev 0x%x ino %llu "
	        "offset 0x%llx", who, (unsigned long long)addr, (unsigned long long)s->vaddr,
	        (unsigned long long)s->size, s->flags, mapinfo::flags_str(s->flags).c_str(), s->dev,
	        (unsigned long long)s->ino, (unsigned long long)s->offset);
	log_msg("maps: %s: %s", who, mapinfo::write_verdict(s->flags).c_str());
}

void dump_segs(FILE *f, const char *who, const std::vector<mapinfo::Seg> &segs)
{
	std::fprintf(f, "=== %s: %u segments\n", who, (unsigned)segs.size());
	for (const mapinfo::Seg &s : segs)
		std::fprintf(f, "  0x%08llx %9llu flags 0x%08x dev 0x%08x ino %-12llu off 0x%08llx  %s\n",
		             (unsigned long long)s.vaddr, (unsigned long long)s.size, s.flags, s.dev,
		             (unsigned long long)s.ino, (unsigned long long)s.offset,
		             mapinfo::flags_str(s.flags).c_str());
}

} // namespace

std::string maps(int pid, const char *out_path)
{
	std::vector<mapinfo::Seg> iom, self;
	std::string err = read_maps(pid, iom);
	if (!err.empty()) {
		log_msg("maps: io-media: %s", err.c_str());
		return err;
	}
	std::string serr = read_maps(0, self);
	FILE *f = std::fopen(out_path, "w");
	if (f != nullptr) {
		dump_segs(f, "io-media-nvidia", iom);
		if (serr.empty())
			dump_segs(f, "hello10 (from the SD card)", self);
		std::fclose(f);
	}
	log_msg("maps: io-media %u segments, hello10 %u (list in %s)", (unsigned)iom.size(),
	        (unsigned)self.size(), out_path);
	for (const Word &w : kWords)
		log_seg("io-media width word", w.addr, mapinfo::find(iom, w.addr));
	if (!serr.empty()) {
		log_msg("maps: hello10: %s", serr.c_str());
	} else {
		u64 own = (u64)(usize)&maps;
		log_seg("hello10 own code", own, mapinfo::find(self, own));
	}
	return "";
}

std::string phys(const char *file)
{
	const u64 addr = kWords[0].addr;
	const u64 file_off = addr - kTextVaddr;            // text starts at file offset 0
	const u64 page_off = file_off & ~(u64)(kPage - 1);
	int fd = open(file, O_RDONLY);
	if (fd < 0)
		return errstr(file);
	void *p = mmap64(nullptr, kPage, PROT_READ | PROT_EXEC, MAP_SHARED, fd, (off64_t)page_off);
	int merr = errno;
	close(fd);
	if (p == MAP_FAILED)
		return std::string("mmap ") + file + ": " + std::strerror(merr);
	const u8 *b = (const u8 *)p + (file_off - page_off);
	u16 word = (u16)(b[0] | (b[1] << 8));
	off64_t off = 0;
	usize contig = 0;
	int ofd = -1;
	int rc = posix_mem_offset64((const u8 *)p + (file_off - page_off), 2, &off, &contig, &ofd);
	log_msg("phys: %s mapped SHARED at %p, file offset 0x%llx reads %u; "
	        "posix_mem_offset64 rc=%d (%s) off 0x%llx contig %u fd %d", file, p,
	        (unsigned long long)file_off, word, rc, rc == 0 ? "ok" : std::strerror(rc),
	        (unsigned long long)off, (unsigned)contig, ofd);
	std::vector<mapinfo::Seg> self;
	if (read_maps(0, self).empty())
		log_seg("hello10's mapping of the file", (u64)(usize)p, mapinfo::find(self, (u64)(usize)p));
	munmap(p, kPage);
	return rc == 0 ? "" : std::string("posix_mem_offset64: ") + std::strerror(rc);
}

} // namespace capture

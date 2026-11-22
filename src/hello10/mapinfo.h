/*
 * mapinfo.h - hello10's reading of a QNX mapping (DCMD_PROC_MAPINFO).
 *
 * Pure: names the flag bits of a procfs_mapinfo entry (QNX 6.3.2's
 * <sys/mman.h> values) and says what a /proc/<pid>/as write into such a
 * segment can do. procnto's write handler (openqnx pathmgr/procfs.c,
 * procfs_write) only copies into the target's address space; it never
 * makes a private copy of a page, so a page without PROT_WRITE faults and
 * the write fails with ESRVRFAULT, shared or private alike.
 */
#ifndef HELLO10_MAPINFO_H
#define HELLO10_MAPINFO_H

#include "types.h"

#include <string>
#include <vector>

namespace mapinfo {

// QNX 6.3.2 <sys/mman.h>
constexpr u32 kShared = 0x00000001;
constexpr u32 kPrivate = 0x00000002;
constexpr u32 kTypeMask = 0x0000000f;
constexpr u32 kProtRead = 0x00000100;
constexpr u32 kProtWrite = 0x00000200;
constexpr u32 kProtExec = 0x00000400;
constexpr u32 kPhys = 0x00010000;
constexpr u32 kAnon = 0x00080000;
constexpr u32 kSysram = 0x01000000;

struct Seg {
	u64 vaddr = 0;
	u64 size = 0;
	u32 flags = 0;
	u32 dev = 0;
	u64 offset = 0;
	u64 ino = 0;
};

// "SHARED FIXED ELF NOSYNCFILE r-x": the share type, the other named bits,
// any unnamed bits as hex, then the protection.
std::string flags_str(u32 flags);

// What a /proc/<pid>/as write into this segment does, in one line.
std::string write_verdict(u32 flags);

// The segment holding addr, or nullptr.
const Seg *find(const std::vector<Seg> &segs, u64 addr);

} // namespace mapinfo

#endif

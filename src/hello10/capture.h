/*
 * capture.h - hello10's read-only probe of io-media's capture width.
 *
 * io-media-nvidia captures the DSP's video input at a literal 720 wide
 * (docs/phone-mirroring.md 7.14, step 5). The width is three 16-bit
 * constant-pool words, each read by mov.w. This file reads them through
 * /proc/<pid>/as and tells how their page is mapped. Writing them there is
 * refused (ESRVRFAULT): iomcopy.h patches a copy of the file instead.
 */
#ifndef HELLO10_CAPTURE_H
#define HELLO10_CAPTURE_H

#include "types.h"

#include <string>

namespace capture {

// Find io-media-nvidia's pid with pidin (through proc_shell). The lines
// that matched go to out_path. Returns "" and the pid, or an error.
std::string find_io_media(const char *out_path, int &pid);

// Read the three width words and the mov.w loads that use them, and log
// each one: the word, the instruction's decoded target, and whether the
// target is the word. Returns "" if every read worked.
std::string probe(int pid);

// Read only. io-media's segment list (DCMD_PROC_MAPINFO) and ours, to
// out_path; logs the segment holding each width word and our own code, with
// the flags named and what a /proc write there can do (mapinfo.h).
std::string maps(int pid, const char *out_path);

// Read only. Maps io-media's file (file, MAP_SHARED, PROT_READ) at the
// width word's page in our own process and logs what posix_mem_offset64
// says about it (the physical address when it is plain memory), the word
// read there and our segment's flags. Whether we share io-media's page, and
// whether that page is RAM or flash, decides if it can be patched in place.
std::string phys(const char *file);

} // namespace capture

#endif

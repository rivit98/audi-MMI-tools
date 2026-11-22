/*
 * capture.h - hello10's in-memory test of io-media's capture width.
 *
 * io-media-nvidia captures the DSP's video input at a literal 720 wide
 * (docs/phone-mirroring.md 7.14, step 5). The width is three 16-bit
 * constant-pool words, each read by mov.w. This file reads them, and can
 * change them to 800 in the running process through /proc/<pid>/as.
 * Nothing on flash or the card is written; a reboot undoes it, and the
 * patch is also undone when the test ends.
 */
#ifndef HELLO10_CAPTURE_H
#define HELLO10_CAPTURE_H

#include "types.h"

#include <string>

namespace capture {

constexpr u16 kWidthFrom = 720;   // what io-media-nvidia has
constexpr u16 kWidthTo = 800;     // what the test writes

// Find io-media-nvidia's pid with pidin (through proc_shell). The lines
// that matched go to out_path. Returns "" and the pid, or an error.
std::string find_io_media(const char *out_path, int &pid);

// Read the three width words and the mov.w loads that use them, and log
// each one: the word, the instruction's decoded target, and whether the
// target is the word. Read only. Returns "" if every read worked.
std::string probe(int pid);

// Change the three words from 720 to 800 (each one must read 720 first),
// read them back, and remember what was there. Returns "" on success.
std::string apply(int pid);

// Put back what apply() changed. Safe to call when nothing was applied.
void restore();

bool applied();

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

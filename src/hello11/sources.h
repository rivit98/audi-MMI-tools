// sources.h - hello11's other two inputs, besides ch4 (ipc4.h). Pure, tested
// on the host (tests/test_hello11.cpp).
//
// ch5 (/dev/ipc/ch5, docs/vehicle-data.md §3): the IOC's vehicle-signal
// records, as ndr reads them (CProcessCh5::interpretBuffer, ndr 0x08063208):
//
//   tag u8 | subtag u8 | len u16 LE | payload[len] | ts u16 LE
//
// Tag 0x09 is the rain sensor (ndr's "RSens"): maybe the light sensor too.
//
// slog (/dev/slog): text lines that may name the display's brightness or the
// day / night state, so a step can be matched with what the HMI did.
#ifndef HELLO11_SOURCES_H
#define HELLO11_SOURCES_H

#include "types.h"

#include <string>
#include <vector>

namespace ch5 {

struct Record {
	u8 tag = 0, subtag = 0;
	std::vector<u8> payload;
	u16 ts = 0;
};

// One read()'s bytes -> records. A record that doesn't fit (a truncated
// tail, or a length past the end) ends the parse: those bytes are returned
// in *skipped.
std::vector<Record> parse(const u8 *p, usize n, usize *skipped);

// Tags that change all the time on their own (gyro, accelerometer, the
// clock): counted, not watched.
bool noisy(u8 tag);

// "ch5 09.00 RainSens"
std::string key(const Record &r);

} // namespace ch5

namespace slogtext {

// Text lines of /dev/slog events as read() gives them, whole: 3 ints of
// header (flags and int count, code, time), then the data (common/camera_log.h).
std::vector<std::string> lines(const int *ev, usize n_ints);

// A line about the display's brightness, dimming, day / night or lights.
bool interesting(const std::string &line);

// The line with every run of digits replaced by '#': lines that differ only
// in their numbers share a key, and the numbers are the value.
std::string key(const std::string &line);
std::string numbers(const std::string &line);

} // namespace slogtext

#endif

// ipc4.h - /dev/ipc/ch4, the IOC's message channel to MMI3GApplication's
// MOST subsystem (docs/vehicle-data.md §5): telegram framing, the MOST-style
// header inside, and the BAP streams some of them carry. Pure: no car APIs,
// tested on the host (tests/test_camera.cpp, tests/test_hello11.cpp). The
// camera library (camera.h) owns the process's one ch4 reader.
//
// Framing (CHBIpcProtocol, MMI3GApplication 0x08806908):
//
//   counter u8 | type u8 | len u16 LE | body[len - 1] | FA DE
//
// The counter runs +1 per telegram (mod 256). Body of a type-1 telegram:
//
//   00 | fblock u8 | inst u8 | fct u16 BE | op u16 LE | plen u16 LE | FF | payload[plen]
//
// fblock D8 inst 01 fct 0x0500 carries BAP streams, one control unit
// ("LSG") each (CBapCommDevCtrl::processStream 0x081da084):
//
//   lsg u8 | { fid u8 | len u16 LE | payload[len] } ...
#ifndef CAMERA_IPC4_H
#define CAMERA_IPC4_H

#include "types.h"

#include <string>
#include <vector>

namespace ipc4 {

struct Telegram {
	u8 counter = 0;
	u8 type = 0;
	std::vector<u8> body;
};

// Byte stream -> telegrams. Reads may split or join telegrams; bytes that
// don't frame are skipped one at a time until a telegram frames again.
class Splitter {
public:
	void feed(const u8 *p, usize n, std::vector<Telegram> &out);

	u32 telegrams() const { return telegrams_; }
	// Bytes skipped to find the next frame.
	u32 skipped() const { return skipped_; }
	// Telegrams whose counter was not the last one + 1 (the first one of each
	// type doesn't count), and how many counters were missing in all.
	u32 gaps() const { return gaps_; }
	u32 missing() const { return missing_; }

private:
	// The frame's length at buf_[at] (at least 4 bytes there): 0 not a frame,
	// -1 not complete yet.
	isize frame_at(usize at) const;

	std::vector<u8> buf_;
	u32 telegrams_ = 0, skipped_ = 0, gaps_ = 0, missing_ = 0;
	int last_[256];
	bool init_ = false;
};

struct Most {
	u8 fblock = 0, inst = 0;
	u16 fct = 0, op = 0;
	std::vector<u8> payload;
};

// false: the body isn't shaped like the header above.
bool parse_most(const std::vector<u8> &body, Most &out);

// "DA01.0C0D/C" (fblock, instance, function, op type in hex).
std::string most_key(const Most &m);

constexpr u8 kBapFblock = 0xD8;
constexpr u16 kBapFct = 0x0500;
bool is_bap(const Most &m);

struct BapFn {
	u8 lsg = 0, fid = 0;
	std::vector<u8> payload;
};

// The functions in one BAP stream; false (out holds what parsed) if it is
// too short, the lsg is 0 or a length runs past the end.
bool parse_bap(const std::vector<u8> &stream, std::vector<BapFn> &out);

// "BAP 0B.17" (lsg, fid).
std::string bap_key(const BapFn &f);

// "0c 0a 01" (at most max bytes, then "..").
std::string hex(const std::vector<u8> &v, usize max = 64);
std::string hex(const u8 *p, usize n, usize max = 64);

// The car-service function block (docs/vehicle-data.md §5.1-5.2):
// InteriorLight status, whose first byte MMI3GApplication passes on as the
// HMI's externLightActive (night map). The camera's BAP functions: bap.h.
constexpr u8 kCarFblock = 0xDA;
constexpr u16 kInteriorLightFct = 0x0C0D;
constexpr u16 kExteriorLightFct = 0x0C0C;

} // namespace ipc4

#endif

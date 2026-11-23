// bap.h - the rear camera's session as its control unit announces it over
// BAP on /dev/ipc/ch4 (docs/rear-camera.md §3.3). Pure, tests/test_camera.cpp.
//
// LSG 0x0B (the camera) function 0x1A, 4 bytes, sent only when it changes:
//
//   f1 01 01 01   session starting      11 01 01 01   session on
//   f0 00 00 00   session ending        00 00 00 00   off
//
// The low nibble of the first byte is the state. On the car it comes
// 0.9-1.3 s before the HMI takes VCAP 0 (the slog's "VP0: source changed")
// and 0.6-1.1 s before it lets go ("VP1: vcap disabled").
#ifndef CAMERA_BAP_H
#define CAMERA_BAP_H

#include "ipc4.h"
#include "types.h"

#include <vector>

namespace camera {

constexpr u8 kRvcLsg = 0x0B;
constexpr u8 kRvcSessionFid = 0x1A;

enum class Rvc { None, On, Off };

// The camera's session state in one BAP function, if it is that function.
inline Rvc rvc_state(const ipc4::BapFn &f)
{
	if (f.lsg != kRvcLsg || f.fid != kRvcSessionFid || f.payload.empty())
		return Rvc::None;
	return (f.payload[0] & 0x0f) != 0 ? Rvc::On : Rvc::Off;
}

// The last camera state in one ch4 telegram (None: it carries none).
inline Rvc rvc_state(const ipc4::Telegram &t)
{
	ipc4::Most m;
	if (t.type != 1 || !ipc4::parse_most(t.body, m) || !ipc4::is_bap(m))
		return Rvc::None;
	std::vector<ipc4::BapFn> fns;
	ipc4::parse_bap(m.payload, fns);     // what parsed, even if not all of it
	Rvc r = Rvc::None;
	for (const ipc4::BapFn &f : fns)
		if (rvc_state(f) != Rvc::None)
			r = rvc_state(f);
	return r;
}

} // namespace camera

#endif

// camera.h - is the rear camera up right now? Two reader threads, reading
// only (nothing here touches the video path), feed state.h's Combiner:
//
//   slog   dev-videoctrl's lines in /dev/slog (vcap.h): the HMI owns VCAP 0
//   ch4    /dev/ipc/ch4 (ipc4.h): the camera's BAP session (bap.h), ~1 s
//          earlier
//
// README.md next to this file has the whole story; docs/rear-camera.md §9.1
// the why.
//
// Not started (or no /dev/slog): active() is false and clear() true, so the
// callers behave as if there were no camera. No ch4: the slog alone, as
// before BAP was known.
//
// ch4 is read through ipc.h, the process's one reader per channel. This
// library splits the reads into telegrams; code that wants ch4's telegrams
// subscribes here (subscribe_ch4), code that wants the raw reads subscribes
// to ipc.h's channel 4.
#ifndef CAMERA_CAMERA_H
#define CAMERA_CAMERA_H

#include "ipc4.h"
#include "types.h"

#include <functional>
#include <vector>

namespace camera {

// Reads the slog so far before it returns, so active() is right at once.
// Opens ch4 unless with_ch4 is false.
void start(bool with_ch4 = true);
// Stops the slog thread and leaves ch4 (ipc.h keeps reading it).
void stop();

// The camera is up (or about to be): don't write VP1, don't show anything.
bool active();
// Not active, and the HMI has had time to show its next context.
bool clear();

struct Status {
	bool active = false;
	bool vcap = false;            // the HMI owns VCAP 0 (slog)
	bool bap = false;             // the camera's BAP session (ch4)
	u32 bap_unconfirmed = 0;      // BAP "on"s the HMI never followed
};
Status status();

// A change of active(). why: "BAP", "VCAP 0", "BAP alone" (an unconfirmed
// BAP "on" dropped), a literal.
struct Event {
	bool up = false;
	const char *why = "";
	u32 ms = 0;                   // log_ms()
};

// Callbacks run on a reader thread with the library's subscriber lock held:
// keep them short, never block, never subscribe / unsubscribe from inside
// one. unsubscribe returns once no callback of that id is running. name: for
// the log, a literal (NULL: "#<id>").
using Handler = std::function<void(const Event &)>;
int subscribe(Handler h, const char *name = nullptr);
void unsubscribe(int id);

// Every ch4 read: the raw bytes as read and the telegrams they completed.
using Ch4Handler =
	std::function<void(u32 ms, const u8 *raw, usize n, const std::vector<ipc4::Telegram> &ts)>;
int subscribe_ch4(Ch4Handler h, const char *name = nullptr);
void unsubscribe_ch4(int id);

struct Ch4Stats {
	const char *state = "not started";  // "reading", "no open", "read failed", "stopped: gaps"
	u32 telegrams = 0, gaps = 0, missing = 0, skipped = 0;
};
Ch4Stats ch4_stats();

} // namespace camera

#endif

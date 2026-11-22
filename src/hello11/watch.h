// watch.h - which values change during which step. hello11 feeds every value
// it reads (a ch4 telegram's payload, a BAP function, the ndr node, the
// camera's slog state) under a key, and the steps the driver goes through.
// The summary names, for each kind of step, the keys that changed in every
// step of that kind and in no quiet step: the signal for that action.
// Pure, tested on the host (tests/test_hello11.cpp).
#ifndef HELLO11_WATCH_H
#define HELLO11_WATCH_H

#include "types.h"

#include <map>
#include <string>
#include <vector>

namespace watch {

struct Change {
	unsigned ms;
	std::string key;
	std::vector<u8> from, to;
};

// A step of the test: [begin_ms, end_ms). kind "" = quiet (a baseline: what
// changes here changes on its own).
struct Step {
	std::string label, kind;
	unsigned begin_ms = 0, end_ms = 0;
};

class Watch {
public:
	// The key's value now. True if it changed (the first value is not a change).
	bool value(unsigned ms, const std::string &key, const std::vector<u8> &v);

	// Steps follow each other: begin() ends the open step.
	void begin(unsigned ms, const std::string &label, const std::string &kind);
	void end(unsigned ms);

	const std::vector<Change> &changes() const { return changes_; }
	const std::vector<Step> &steps() const { return steps_; }
	usize keys() const { return last_.size(); }

	// Keys changed in [b, e).
	std::vector<std::string> changed_in(unsigned b, unsigned e) const;

	// Per kind: keys changed in every step of that kind and in no quiet step.
	std::map<std::string, std::vector<std::string>> candidates() const;

	// The changes of one key inside [b, e), "a -> b" each.
	std::vector<std::string> describe(const std::string &key, unsigned b, unsigned e) const;

private:
	std::map<std::string, std::vector<u8>> last_;
	std::vector<Change> changes_;
	std::vector<Step> steps_;
	bool open_ = false;
};

} // namespace watch

#endif

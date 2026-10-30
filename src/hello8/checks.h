// checks.h - hello8's C++ checks: the language runtime (static init,
// exceptions, RTTI) and the standard library (containers, algorithms,
// strings, smart pointers, math, random, chrono, threads), as built by
// shle-nto-g++ for the car.
//
// Pure C++, nothing from the MMI: tests/test_hello8.cpp runs the same checks
// on the build host, which proves the expectations themselves.
#ifndef HELLO8_CHECKS_H
#define HELLO8_CHECKS_H

#include <string>
#include <vector>

namespace checks {

struct Result {
	std::string name;
	bool ok = true;
	// The first expectation that failed ("line N: expr"), or when all
	// passed, the check's notes (measurements, sizes).
	std::string detail;
	unsigned ms = 0;
};

struct Check {
	const char *name;
	void (*run)(Result &r);
};

// Every check, in run order: the ones the others rely on first
// (exceptions), the multi-threaded ones last.
const std::vector<Check> &all();

// Runs one check. An exception escaping it is a failure, not a crash.
Result run(const Check &c);

// Call first thing in main(): the static-init check tells constructors that
// ran before main from ones that didn't.
void mark_main();

// llcs.cpp, built with SH-4A LL/SC atomics on the car: __atomic_fetch_add on
// *count until *stop, returns how many it did. For "atomics stress".
unsigned long long llcs_add_until(unsigned *count, const bool *stop);

} // namespace checks

#endif

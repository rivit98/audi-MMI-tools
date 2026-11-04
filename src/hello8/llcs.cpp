// llcs.cpp - the LL/SC half of hello8's "atomics stress" check (checks.cpp).
// On the car __atomic_fetch_add becomes SH-4A movli.l/add/movco.l/bf, which
// a thread switch can't break. Until 2026-10-30 this file alone was built
// with -matomic-model=hard-llcs; since then the SDK builds everything that
// way, so it gets the default flags and checks the default. On the build
// host it is the host's ordinary atomics.
#include "checks.h"

#if defined(__SH__) && !defined(__SH_ATOMIC_MODEL_HARD_LLCS__)
#error "the SDK's default atomics model should be hard-llcs (sdk/shle-nto-gcc)"
#endif

namespace checks {

unsigned long long llcs_add_until(unsigned *count, const bool *stop)
{
	unsigned long long n = 0;

	while (!__atomic_load_n(stop, __ATOMIC_RELAXED)) {
		__atomic_fetch_add(count, 1u, __ATOMIC_RELAXED);
		n++;
	}
	return n;
}

} // namespace checks

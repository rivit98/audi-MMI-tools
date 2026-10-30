// llcs.cpp - the LL/SC half of hello8's "atomics stress" check (checks.cpp).
// On the car this one file is built with -m4a -matomic-model=hard-llcs
// (Makefile), so __atomic_fetch_add becomes SH-4A movli.l/add/movco.l/bf,
// which a thread switch can't break, instead of the gUSA sequence the rest
// of hello8 gets. On the build host it is the host's ordinary atomics.
#include "checks.h"

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

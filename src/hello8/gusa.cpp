// gusa.cpp - the gUSA half of hello8's "atomics stress" check (checks.cpp).
// The SDK builds everything with SH-4A LL/SC atomics;
// this one file is built with GCC's old sh4 default, -matomic-model=
// soft-gusa,strict (Makefile; without strict, -m4a turns 32-bit atomics
// into LL/SC anyway), so the check keeps showing what that model does on
// QNX (it lost 9 % of its updates on the car). On the build host it is the
// host's ordinary atomics.
#include "checks.h"

#if defined(__SH__) && !defined(__SH_ATOMIC_MODEL_SOFT_GUSA__)
#error "gusa.cpp must be built with -matomic-model=soft-gusa,strict (Makefile GUSA_FLAGS)"
#endif

namespace checks {

unsigned long long gusa_add_until(unsigned *count, const bool *stop)
{
	unsigned long long n = 0;

	while (!__atomic_load_n(stop, __ATOMIC_RELAXED)) {
		__atomic_fetch_add(count, 1u, __ATOMIC_RELAXED);
		n++;
	}
	return n;
}

} // namespace checks

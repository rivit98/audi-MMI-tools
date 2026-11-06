// test_hello8.cpp - hello8's checks (src/hello8/checks.cpp) on the build
// host with glibc's libstdc++: every one must pass here, so a failure on the
// car is the car's C++ runtime, not a wrong expectation.
#include "check.h"
#include "checks.h"

// new/delete asks for half the address space and expects nullptr /
// std::bad_alloc, which ASan reports as an error by default.
extern "C" const char *__asan_default_options()
{
	return "allocator_may_return_null=1";
}

int main()
{
	checks::mark_main();
	for (const checks::Check &c : checks::all()) {
		checks::Result r = checks::run(c);
		printf("%s %-14s %5u ms  %s\n", r.ok ? "pass" : "FAIL", r.name.c_str(), r.ms,
		       r.detail.c_str());
		CHECK(r.ok);
	}
	CHECK_EQ(checks::all().size(), 19);
	CHECK_DONE();
}

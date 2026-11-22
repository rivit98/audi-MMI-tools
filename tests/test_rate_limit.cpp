// test_rate_limit.cpp - the warnings' rate limit (src/common/rate_limit.h).
#include "check.h"
#include "rate_limit.h"

int main(void)
{
	// the warnings' limit: once per period, the rest counted
	{
		RateLimit r(10000);
		unsigned held = 99;
		CHECK(r.allow(500, &held));
		CHECK_EQ(held, 0);
		CHECK(!r.allow(600));
		CHECK(!r.allow(10499));
		CHECK_EQ(r.held(), 2);
		CHECK(r.allow(10500, &held));
		CHECK_EQ(held, 2);
		CHECK_EQ(r.held(), 0);
		CHECK(!r.allow(10501));
		// across the wrap
		RateLimit w(1000);
		CHECK(w.allow(0xfffffff0u));
		CHECK(!w.allow(100));
		CHECK(w.allow(0xfffffff0u + 1000, &held));
		CHECK_EQ(held, 1);
	}
	// the first call always logs, whatever the time
	{
		RateLimit r(10000);
		CHECK(r.allow(0));
	}
	CHECK_DONE();
}

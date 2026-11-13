// test_pacer.cpp - audi_stream_receiver's JPEG cap (src/audi_stream_receiver/
// jpeg_pacer.h) and the warnings' rate limit (src/common/rate_limit.h).
#include "check.h"
#include "jpeg_pacer.h"
#include "rate_limit.h"

int main()
{
	constexpr unsigned kMin = JpegPacer::kMinIntervalMs;

	CHECK_EQ(JpegPacer::kMaxJpegFps, 5);
	CHECK_EQ(kMin, 200);
	// nothing taken yet (a new stream): the first picture goes at once
	{
		JpegPacer p;
		CHECK(p.ready(0));
		CHECK(p.ready(123456));
		CHECK_EQ(p.wait_ms(5), 0);
		CHECK(!p.active());
	}
	// cheap pictures: the rate limit, 200 ms from take to take
	{
		JpegPacer p;
		p.taken(1000);
		CHECK(!p.ready(1000));
		p.done(1050, 50);              // 50 ms busy: rest to 1100, rate to 1200
		CHECK(!p.ready(1199));
		CHECK_EQ(p.wait_ms(1150), 50);
		CHECK(p.ready(1200));
		CHECK_EQ(p.wait_ms(1200), 0);
		CHECK(p.active());
	}
	// at most 5 a second, however fast they come
	{
		JpegPacer p;
		unsigned taken = 0;
		for (unsigned t = 0; t < 10000; t += 10) {
			if (p.ready(t)) {
				p.taken(t);
				p.done(t + 40, 40);
				taken++;
			}
		}
		CHECK_EQ(taken, 50);
	}
	// an expensive picture: as much rest as it took, so ~50 % duty
	{
		JpegPacer p;
		p.taken(1000);
		p.done(1250, 250);            // full-screen change: rest until 1500
		CHECK(!p.ready(1499));
		CHECK(p.ready(1500));
		CHECK_EQ(p.last_busy_ms(), 250);
		unsigned busy = 0, t = 0;
		JpegPacer q;
		for (; t < 10000; t++) {
			if (q.ready(t)) {
				q.taken(t);
				q.done(t + 250, 250);
				busy += 250;
				t += 249;
			}
		}
		CHECK(busy * 100 / t <= 51);
	}
	// a very long frame holds the next for kMaxBusyMs at most (the phone's
	// ack timeout is 3 s)
	{
		JpegPacer p;
		p.taken(0);
		p.done(5000, 5000);
		CHECK(p.ready(5000 + JpegPacer::kMaxBusyMs));
		CHECK(!p.ready(5000 + JpegPacer::kMaxBusyMs - 1));
	}
	// across log_ms()'s wrap
	{
		JpegPacer p;
		p.taken(0xffffff00u);
		p.done(0xffffff40u, 64);       // rest to 0xffffff80, rate to 0xffffffc8
		CHECK(!p.ready(0xffffff80u));
		CHECK(!p.ready(0xffffffc7u));
		CHECK(p.ready(0xffffff00u + kMin));
		CHECK(p.ready(5));
		CHECK_EQ(p.wait_ms(0xffffffc0u), 8);
	}
	// done() before any taken(): ignored
	{
		JpegPacer p;
		p.done(100, 500);
		CHECK(p.ready(100));
	}
	// reset: a new stream starts at once
	{
		JpegPacer p;
		p.taken(1000);
		p.done(1100, 100);
		CHECK(!p.ready(1150));
		p.reset();
		CHECK(p.ready(1150));
	}

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

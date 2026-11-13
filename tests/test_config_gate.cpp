// test_config_gate.cpp - which H264_CONFIGs restart the MMI's stream and when
// a frame asks the phone for one (src/audi_stream_receiver/config_gate.h).
#include "check.h"
#include "config_gate.h"

#include "types.h"
#include <vector>

int main()
{
	const std::vector<u8> a = {1, 2, 3}, b = {1, 2, 4};

	{
		ConfigGate g;
		CHECK(!g.taken());
		CHECK(g.current().empty());
	}
	// one restart per config, however many copies come while it starts
	{
		ConfigGate g;
		CHECK(g.offer(a));
		CHECK(g.taken());
		CHECK(!g.offer(a));
		CHECK(!g.offer(a));
		CHECK(g.current() == a);
	}
	// a different config, and going back to the old one, are new
	{
		ConfigGate g;
		CHECK(g.offer(a));
		CHECK(g.offer(b));
		CHECK(g.current() == b);
		CHECK(g.offer(a));
		CHECK(g.current() == a);
	}
	// nothing running or coming any more: the same config starts a stream
	{
		ConfigGate g;
		CHECK(g.offer(a));
		g.forget();
		CHECK(!g.taken());
		CHECK(g.current().empty());
		CHECK(g.offer(a));
	}
	// asking for the config: once per period, not once per frame
	{
		ConfigGate g;
		CHECK(g.ask(1000));
		CHECK(!g.ask(1040));
		CHECK(!g.ask(1499));
		CHECK(g.ask(1500));
		CHECK(!g.ask(1501));
	}
	// the time 0 and the clock wrapping are nothing special
	{
		ConfigGate g;
		CHECK(g.ask(0));
		CHECK(!g.ask(1));
		ConfigGate w;
		CHECK(w.ask(0xfffffff0u));
		CHECK(!w.ask(0xfffffff8u));
		CHECK(w.ask(0xfffffff0u + ConfigGate::kAskEveryMs));
	}
	// a config answers the question; the next one to go missing asks at once
	{
		ConfigGate g;
		CHECK(g.ask(1000));
		CHECK(g.offer(a));
		g.forget();
		CHECK(g.ask(1001));
	}
	CHECK_DONE();
}

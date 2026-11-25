// test_quit.cpp - quit's stop sources and its cleanup (src/common/quit/),
// make test. One process, so one quit::start: the steps run in order. The
// hard path (_exit) and the ch8 stop key need a car.
#include "check.h"
#include "types.h"

#include <cstdlib>
#include <unistd.h>

extern "C" {
#include "log.h"
}
#include "quit.h"

int main()
{
	char card[] = "/tmp/test_quit_card_XXXXXX";
	int cleanups = 0;
	bool cleanup_hard = true;

	log_open("/dev/null");
	CHECK(mkdtemp(card) != nullptr);
	quit::Config qc;
	qc.sd_path = card;
	qc.run_ms = 400;
	qc.cleanup = [&](bool hard) {
		cleanups++;
		cleanup_hard = hard;
	};
	CHECK(quit::start(qc));

	// nothing asked for yet
	CHECK(quit::poll() == quit::Reason::None);
	CHECK_STR(quit::name(quit::Reason::SdCard), "SD card removed");

	// the timer, with the card still there
	unsigned t0 = log_ms();
	while (!quit::stopping() && log_ms() - t0 < 3000)
		usleep(20 * 1000);
	CHECK(quit::poll() == quit::Reason::Timer);
	CHECK(log_ms() - t0 >= 300);

	// the first reason stays
	quit::request(quit::Reason::Signal);
	CHECK(quit::poll() == quit::Reason::Timer);

	// the cleanup runs once, on this thread, not hard
	CHECK(quit::finish() == quit::Reason::Timer);
	CHECK_EQ(cleanups, 1);
	CHECK(!cleanup_hard);
	quit::finish();
	CHECK_EQ(cleanups, 1);
	rmdir(card);

	CHECK_DONE();
}

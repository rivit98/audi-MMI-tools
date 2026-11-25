/*
 * test_quit.c - quit.c's stop sources and its cleanup (src/common/quit/),
 * make test. One process, so one quit_start: the steps run in order. The
 * hard path (_exit) and the ch8 stop key need a car.
 */
#define _GNU_SOURCE
#include "types.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "check.h"
#include "log.h"
#include "quit.h"

static int cleanups, cleanup_hard = -1;
static int arg_seen;

static void on_quit(int hard, void *arg)
{
	cleanups++;
	cleanup_hard = hard;
	arg_seen = arg == &arg_seen;
}

int main(void)
{
	char card[] = "/tmp/test_quit_card_XXXXXX";
	struct quit_config qc = { 0 };
	unsigned t0;

	log_open("/dev/null");
	CHECK(mkdtemp(card) != NULL);
	qc.sd_path = card;
	qc.run_ms = 400;
	qc.cleanup = on_quit;
	qc.arg = &arg_seen;
	CHECK_EQ(quit_start(&qc), 0);

	/* nothing asked for yet */
	CHECK_EQ(quit_poll(), QUIT_NONE);
	CHECK_STR(quit_name(QUIT_SDCARD), "SD card removed");

	/* the timer, with the card still there */
	t0 = log_ms();
	while (quit_poll() == QUIT_NONE && log_ms() - t0 < 3000)
		usleep(20 * 1000);
	CHECK_EQ(quit_poll(), QUIT_TIMER);
	CHECK(log_ms() - t0 >= 300);

	/* the first reason stays */
	quit_request(QUIT_SIGNAL);
	CHECK(quit_poll() != QUIT_SIGNAL);

	/* the cleanup runs once, on this thread, not hard */
	CHECK_EQ(quit_finish(), QUIT_TIMER);
	CHECK_EQ(cleanups, 1);
	CHECK_EQ(cleanup_hard, 0);
	CHECK_EQ(arg_seen, 1);
	quit_finish();
	CHECK_EQ(cleanups, 1);
	rmdir(card);

	CHECK_DONE();
}

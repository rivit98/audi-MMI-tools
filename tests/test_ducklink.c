/*
 * test_ducklink.c - hello5 step d's helper link (src/hello5/ducklink.c):
 * line parsing, and real helpers (shell scripts) that answer, hang, exit
 * or are told to stop. A hanging helper must cost its deadline and no
 * more, and link_end must not block on it.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "types.h"
#include "ducklink.h"

static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000);
}

/* A fake helper: answers on fd 3 like duckhelper.c. */
static const char script[] =
	"while read cmd; do case $cmd in "
	"quit) exit 0;; "
	"connect) echo 'wait 50 broker' >&3; echo 'note chid 14' >&3; "
	"         echo 'ok 123 14 0x00020027.0' >&3;; "
	"slow) echo 'wait 50 a' >&3; sleep 1; echo 'wait 1500 b' >&3; sleep 2; "
	"      echo 'ok late' >&3;; "
	"hang) echo 'wait 100 ConnectRequest' >&3; exec sleep 30;; "
	"bye) exit 0;; "
	"*) echo 'err unknown command' >&3;; "
	"esac; done";

static int start(struct duck_link *l)
{
	char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)script, NULL };

	return link_start(l, argv);
}

static void test_parse(void)
{
	const char *rest;
	unsigned ms = 0;

	CHECK_EQ(link_parse("ok 1 2", &ms, &rest), LINK_OK);
	CHECK_STR(rest, "1 2");
	CHECK_EQ(link_parse("ok", &ms, &rest), LINK_OK);
	CHECK_STR(rest, "");
	CHECK_EQ(link_parse("err no broker", &ms, &rest), LINK_ERR);
	CHECK_STR(rest, "no broker");
	CHECK_EQ(link_parse("wait 1600 ConnectRequest to chid 14", &ms, &rest), -1);
	CHECK_EQ(ms, 1600u);
	CHECK_STR(rest, "ConnectRequest to chid 14");
	CHECK_EQ(link_parse("wait x", &ms, &rest), -3);
	CHECK_EQ(link_parse("note attached", &ms, &rest), -2);
	CHECK_STR(rest, "attached");
	CHECK_EQ(link_parse("okay", &ms, &rest), -3);
	CHECK_EQ(link_parse("", &ms, &rest), -3);
}

/* the progress callback: what it saw last */
static unsigned progress_calls;
static char progress_what[96];

static void on_progress(const struct duck_link *l, unsigned now)
{
	(void)now;
	progress_calls++;
	snprintf(progress_what, sizeof(progress_what), "%s", l->what);
}

static void test_answers(void)
{
	struct duck_link l;
	char reply[64];
	unsigned t0;

	CHECK_EQ(start(&l), 0);
	l.progress = on_progress;
	CHECK_EQ(link_cmd(&l, "connect", 100, reply, sizeof(reply), NULL), LINK_OK);
	CHECK_STR(reply, "123 14 0x00020027.0");
	CHECK_STR(l.note, "chid 14");
	CHECK_EQ(link_cmd(&l, "what", 100, reply, sizeof(reply), NULL), LINK_ERR);
	CHECK_STR(reply, "unknown command");
	/* each "wait" moves the deadline: 3 s in all, more than the first
	 * deadline (50 + slack) */
	t0 = now_ms();
	CHECK_EQ(link_cmd(&l, "slow", 0, reply, sizeof(reply), NULL), LINK_OK);
	CHECK_STR(reply, "late");
	CHECK(now_ms() - t0 >= 2900);
	/* called while waiting, with the helper's last "wait" */
	CHECK(progress_calls >= 10);
	CHECK_STR(progress_what, "b");
	CHECK_STR(l.what, "b");
	t0 = now_ms();
	CHECK_EQ(link_end(&l, 2000), 1);
	CHECK(now_ms() - t0 < 1000);
	CHECK_EQ(l.pid, 0);
	CHECK_EQ(link_cmd(&l, "connect", 100, reply, sizeof(reply), NULL), LINK_GONE);
}

static void test_hang(void)
{
	struct duck_link l;
	char reply[64];
	unsigned t0, dt;

	CHECK_EQ(start(&l), 0);
	t0 = now_ms();
	CHECK_EQ(link_cmd(&l, "hang", 100, reply, sizeof(reply), NULL), LINK_TIMEOUT);
	dt = now_ms() - t0;
	/* the helper's own "wait 100" + slack, not its 30 s */
	CHECK(dt >= 100 + DUCK_LINK_SLACK_MS - 50);
	CHECK(dt < 100 + DUCK_LINK_SLACK_MS + 1000);
	CHECK(l.dead);
	CHECK_EQ(link_cmd(&l, "connect", 100, reply, sizeof(reply), NULL), LINK_GONE);
	t0 = now_ms();
	CHECK_EQ(link_end(&l, 3000), 1);       /* SIGKILL works on the host */
	CHECK(now_ms() - t0 < 1000);           /* no grace for a dead helper */
}

static void test_gone(void)
{
	struct duck_link l;
	char reply[64];

	CHECK_EQ(start(&l), 0);
	CHECK_EQ(link_cmd(&l, "bye", 1000, reply, sizeof(reply), NULL), LINK_GONE);
	CHECK_EQ(link_cmd(&l, "connect", 100, reply, sizeof(reply), NULL), LINK_GONE);
	CHECK_EQ(link_end(&l, 1000), 1);
}

static int stop_now(void)
{
	return 1;
}

static void test_stop(void)
{
	struct duck_link l;
	char reply[64];
	unsigned t0;

	CHECK_EQ(start(&l), 0);
	t0 = now_ms();
	CHECK_EQ(link_cmd(&l, "hang", 100, reply, sizeof(reply), stop_now), LINK_STOP);
	CHECK(now_ms() - t0 < 500);
	/* stuck in its sleep: quit unread, so SIGKILL after the grace */
	t0 = now_ms();
	CHECK_EQ(link_end(&l, 300), 1);
	CHECK(now_ms() - t0 < 1500);
}

static void test_no_helper(void)
{
	struct duck_link l;
	char *argv[] = { (char *)"/nonexistent/hello5", NULL };
	char reply[8];

	CHECK(link_start(&l, argv) != 0);
	CHECK_EQ(link_cmd(&l, "connect", 100, reply, sizeof(reply), NULL), LINK_GONE);
	CHECK_EQ(link_end(&l, 100), 1);
}

int main(void)
{
	test_parse();
	test_answers();
	test_hang();
	test_gone();
	test_stop();
	test_no_helper();
	CHECK_DONE();
}

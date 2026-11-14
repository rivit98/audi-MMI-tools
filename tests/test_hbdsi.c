/*
 * test_hbdsi.c - src/common/hbdsi_wire.c: the byte layout of the broker
 * attach and of the HBDSI messages, at the offsets the unit's own code
 * reads them (servicebroker, MMI3GApplication; docs/hbdsi.md), and the
 * broker listing parser.
 */
#include "types.h"
#include <string.h>

#include "check.h"
#include "hbdsi_wire.h"

static u32 rd32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u16 rd16(const u8 *p)
{
	return (u16)(p[0] | p[1] << 8);
}

static const struct hbdsi_attach_reply reply = {
	0x11, 0, 0x2a00b, 3, { 0x101, 0x202 }, { 7, 0x202 }
};

static void test_attach_msg(void)
{
	struct hbdsi_attach_msg m;
	const u8 *b = (const u8 *)&m;
	char longname[300];

	CHECK_EQ(hbdsi_attach_msg_init(&m, "SGAudioManagement", 1, 2), 0);
	/* the broker: header (0, <= 1), version at +4/+6, name at +8 */
	CHECK_EQ(rd16(b), 0);
	CHECK_EQ(rd16(b + 2), 1);
	CHECK_EQ(rd16(b + 4), 1);
	CHECK_EQ(rd16(b + 6), 2);
	CHECK_STR((const char *)b + 8, "SGAudioManagement");
	CHECK_EQ(sizeof(m), 0x108);

	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	CHECK_EQ(hbdsi_attach_msg_init(&m, longname, 1, 0), -1);
}

/* The header MMI3GApplication builds (FUN_0806a364, FUN_08094328). */
static void check_header(const u8 *b, u32 cmd, u32 len)
{
	CHECK_EQ(rd32(b), 0x200);
	CHECK_EQ(rd16(b + 4), 0);
	CHECK_EQ(rd16(b + 6), 1);
	CHECK_EQ(rd32(b + 0x08), 0x101);        /* attach reply +0x10 */
	CHECK_EQ(rd32(b + 0x0c), 0x202);        /* +0x14 */
	CHECK_EQ(rd32(b + 0x10), 7);            /* +0x18 */
	CHECK_EQ(rd32(b + 0x14), 0x202);        /* +0x1c */
	CHECK_EQ(rd32(b + 0x18), cmd);
	CHECK_EQ(rd32(b + 0x1c), len);
	CHECK_EQ(rd32(b + 0x20), len);
	CHECK_EQ(rd32(b + 0x24), 0);
}

static void test_connect(void)
{
	const struct hbdsi_connect mine = { { 5, 6 }, 0x1234, 0 };
	u8 b[0x38];

	memset(b, 0xee, sizeof(b));
	CHECK_EQ(hbdsi_build_connect(b, sizeof(b), &reply, &mine), 0x38);
	check_header(b, HBDSI_CMD_CONNECT, 0x10);
	/* the server reads our chids, pid and nd at +0x28..+0x34 */
	CHECK_EQ(rd32(b + 0x28), 5);
	CHECK_EQ(rd32(b + 0x2c), 6);
	CHECK_EQ(rd32(b + 0x30), 0x1234);
	CHECK_EQ(rd32(b + 0x34), 0);
	CHECK_EQ(hbdsi_build_connect(b, 0x37, &reply, &mine), -1);
}

static void test_request(void)
{
	const i32 args[2] = { SGAM_SRC_NAVTTS, SGAM_SNK_SPEAKERS };
	u8 b[0x40];

	memset(b, 0xee, sizeof(b));
	CHECK_EQ(hbdsi_build_request(b, sizeof(b), &reply, SGAM_REQ_AUDIO_FOCUS,
	                             args, 2), 0x40);
	check_header(b, HBDSI_CMD_DATA_REQUEST, 0x18);
	/* the request event's base: the receiver checks kind 0x100 at +0x2c */
	CHECK_EQ(rd32(b + 0x28), 0);
	CHECK_EQ(rd32(b + 0x2c), 0x100);
	CHECK_EQ(rd32(b + 0x30), 2);            /* RequestAudioFocus */
	CHECK_EQ(rd32(b + 0x34), 0);
	/* its arguments, int32 each (FUN_0810fd90): source, sink */
	CHECK_EQ(rd32(b + 0x38), 55);
	CHECK_EQ(rd32(b + 0x3c), 0);
	CHECK_EQ(hbdsi_build_request(b, 0x3f, &reply, 2, args, 2), -1);

	CHECK_EQ(hbdsi_build_request(b, sizeof(b), &reply, SGAM_REQ_RELEASE_FOCUS,
	                             args, 2), 0x40);
	CHECK_EQ(rd32(b + 0x30), 3);
}

/* Shaped like the broker's own "%-8d %-11s  %s  %d.%d" lines. */
static const char listing[] =
	"servicebroker master\n"
	"build: Oct 12 2010\n"
	"Connected Servers:\n"
	" 1        0/2a00b/3    SGAudioManagementClient  1.0\n"
	" 2        0/2a00b/3    SGAudioManagement  1.2\n"
	" 3        0/2a00b/3    SPHAudioManagement  3.0\n"
	"Client Detach Notifications:\n"
	" -4- <2.7>\n";

static void test_listing(void)
{
	char name[64];
	u16 maj = 0, min = 0;

	CHECK_EQ(hbdsi_listing_find(listing, "SGAudioManagement", 0, name,
	                            sizeof(name), &maj, &min), 1);
	CHECK_STR(name, "SGAudioManagementClient");
	CHECK_EQ(maj, 1);
	CHECK_EQ(min, 0);
	CHECK_EQ(hbdsi_listing_find(listing, "SGAudioManagement", 1, name,
	                            sizeof(name), &maj, &min), 1);
	CHECK_STR(name, "SGAudioManagement");
	CHECK_EQ(maj, 1);
	CHECK_EQ(min, 2);
	CHECK_EQ(hbdsi_listing_find(listing, "SGAudioManagement", 2, name,
	                            sizeof(name), &maj, &min), 0);
	CHECK_EQ(hbdsi_listing_find(listing, "SPH", 0, name, sizeof(name),
	                            &maj, &min), 1);
	CHECK_EQ(maj, 3);
	CHECK_EQ(hbdsi_listing_find("", "x", 0, name, sizeof(name), &maj, &min), 0);
	/* no trailing newline, name too long for the buffer */
	CHECK_EQ(hbdsi_listing_find(" 9 a/b/c  SGAudioManagement  2.0", "SGAudio",
	                            0, name, sizeof(name), &maj, &min), 1);
	CHECK_EQ(maj, 2);
	CHECK_EQ(hbdsi_listing_find(" 9 a/b/c  SGAudioManagement  2.0", "SGAudio",
	                            0, name, 8, &maj, &min), 0);
}

static void test_status(void)
{
	CHECK_STR(hbdsi_broker_status_name(0), "ok");
	CHECK_STR(hbdsi_broker_status_name(2), "unknown interface");
	CHECK_STR(hbdsi_broker_status_name(99), "unknown status");
}

int main(void)
{
	test_attach_msg();
	test_connect();
	test_request();
	test_listing();
	test_status();
	CHECK_DONE();
}

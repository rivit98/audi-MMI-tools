/*
 * test_tpad.c - src/hello7/tpad.c: touch pad subfunctions and steering
 * wheel bytes, on messages logged on the car (src/common/buttons/README.md)
 * and on the layouts MMI3GApplication's KeyPanel reads.
 */
#include "check.h"
#include "types.h"
#include "tpad.h"

/* a touch pad message: kind 02, data from byte 27; returns its length */
static int pad(u8 *m, const u8 *data, int n)
{
	memset(m, 0, 48);
	m[14] = 0x02;
	m[15] = 0x02;
	m[17] = 0x01;
	memcpy(m + 27, data, (usize)n);
	return 27 + n;
}

static void one(const u8 *data, int n, const char *text)
{
	u8 m[48];
	char buf[96];
	int len = pad(m, data, n);

	tpad_describe(m, len, buf, sizeof(buf));
	CHECK_STR(buf, text);
}

#define ONE(text, ...) \
	do { \
		static const u8 d[] = { __VA_ARGS__ }; \
		one(d, (int)sizeof(d), text); \
	} while (0)

static void wheel(u8 *m, int src, int c0, int c1, int ev)
{
	memset(m, 0, 48);
	m[14] = 0x01;
	m[15] = 0x02;
	m[16] = (u8)src;
	m[17] = 0x01;
	m[23] = (u8)c0;
	m[24] = (u8)c1;
	m[25] = (u8)ev;
	m[26] = 0x11;
}

int main(void)
{
	u8 m[48], cmd[16];
	char buf[96];
	struct mfl_hit h[MFL_MAX];
	int n;

	/* logged on the car: one stroke in the radio screen */
	ONE("pad press x=132 y=198", 0xca, 0x84, 0xc6);
	ONE("pad key approached KEY6", 0x3d, 0x3c);
	ONE("pad key abandoned KEY4", 0x3e, 0x3a);
	ONE("pad key approached KEY2", 0x3d, 0x3d);
	ONE("pad key approached KEY1", 0x3d, 0x37);
	ONE("pad release", 0xcb);
	ONE("pad mode 6", 0xc4, 0x06);
	/* the other subfunctions, as the KeyPanel reads them */
	ONE("pad key pressed KEY3", 0x30, 0x39);
	ONE("pad key released NE", 0x31, 0x52);
	ONE("pad key approached ? (0x38)", 0x3d, 0x38);
	ONE("pad scroll SW", 0x38, 0x06);
	ONE("pad scroll ?", 0x38, 0x00);
	ONE("pad border CENTER", 0x39, 0x00);
	ONE("pad border NW", 0x39, 0x08);
	ONE("pad slider 2 -3", 0x48, 0x02, 0xfd);
	ONE("pad abs x=10 y=250", 0xc5, 10, 250);
	ONE("pad rel +1 -2", 0xc6, 0x01, 0xfe);
	ONE("pad char \"A\"", 0xc7, 0x00, 0x01, 'A');
	ONE("pad char (not UTF-8) \"\"", 0xc7, 0x00, 0x02);
	ONE("pad tap", 0xd3, 0x00);
	ONE("pad double tap", 0xd3, 0x01);
	ONE("pad language \"deDE\"", 0xc2, 'd', 'e', 'D', 'E', 0);
	ONE("pad setting 3 = 0x00000102", 0xc1, 0, 0, 3, 0, 0, 1, 2);
	ONE("pad sub 0x99", 0x99, 0x01);
	ONE("pad press x=200 y=-1", 0xca, 200);     /* cut short */

	/* subfunction, user vs status, mode echo */
	n = pad(m, (const u8[]){ 0xc4, 0x09 }, 2);
	CHECK_EQ(tpad_sub(m, n), 0xc4);
	CHECK_EQ(tpad_mode_echo(m, n), 9);
	CHECK(!tpad_is_user(0xc4));
	CHECK(!tpad_is_user(0xc2));
	CHECK(tpad_is_user(0xca));
	CHECK(tpad_is_user(0x3d));
	CHECK_EQ(tpad_mode_echo(m, 28), -1);
	m[14] = 0x01;
	CHECK_EQ(tpad_sub(m, n), -1);
	CHECK_EQ(tpad_sub(m, 27), -1);

	/* the HMI's mode command */
	CHECK_EQ(tpad_mode_cmd(6, cmd, 9), 0);
	CHECK_EQ(tpad_mode_cmd(17, cmd, sizeof(cmd)), 10);
	{
		static const u8 want[] = { 0x22, 0, 0, 0, 0, 0, 0x02, 0, 0xc4, 17 };
		CHECK(memcmp(cmd, want, sizeof(want)) == 0);
	}
	CHECK_EQ(tpad_nmodes, 9);

	/* steering wheel, logged on the car: volume roller +1, -1, release */
	wheel(m, 3, 0x12, 0, 0x01);
	n = mfl_decode(m, 27, h, MFL_MAX);
	CHECK_EQ(n, 1);
	CHECK_EQ(h[0].code, 0x12);
	CHECK(h[0].roller);
	CHECK_EQ(h[0].ticks, 1);
	mfl_describe(m, 27, buf, sizeof(buf));
	CHECK_STR(buf, "wheel roller 0x12 +1");
	wheel(m, 3, 0x12, 0, 0x0f);
	mfl_describe(m, 27, buf, sizeof(buf));
	CHECK_STR(buf, "wheel roller 0x12 -1");
	wheel(m, 3, 0, 0, 0);
	CHECK_EQ(mfl_decode(m, 27, h, MFL_MAX), 0);
	mfl_describe(m, 27, buf, sizeof(buf));
	CHECK_STR(buf, "");
	/* two keys at once, source 0; NO_KEY; sources 1/2 and short messages */
	wheel(m, 0, 0x05, 0x06, 0xe1);
	mfl_describe(m, 27, buf, sizeof(buf));
	CHECK_STR(buf, "wheel key 0x05 event 1 + wheel roller 0x06 -2");
	wheel(m, 3, 0x22, 0, 0x01);
	CHECK_EQ(mfl_decode(m, 27, h, MFL_MAX), 0);
	wheel(m, 1, 0x12, 0, 0x01);
	CHECK_EQ(mfl_decode(m, 27, h, MFL_MAX), 0);
	wheel(m, 3, 0x12, 0, 0x01);
	CHECK_EQ(mfl_decode(m, 26, h, MFL_MAX), 0);
	CHECK_EQ(mfl_decode(m, 27, h, 0), 0);
	m[14] = 0x02;
	CHECK_EQ(mfl_decode(m, 27, h, MFL_MAX), 0);
	CHECK_DONE();
}

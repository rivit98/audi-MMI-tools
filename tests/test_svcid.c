/*
 * test_svcid.c - hello5's server-id guesses (src/hello5/svcid.c): which
 * words out of MMI3GApplication's memory can be a component id, and the
 * order they are tried in.
 */
#include <string.h>

#include "check.h"
#include "svcid.h"

static void test_shape(void)
{
	/* the two audio management components of the bootstrap table */
	CHECK(svc_id_shape(SVC_ID_AUDIO_DEVCTRL));
	CHECK(svc_id_shape(SVC_ID_AUDIO_PRESCTRL));
	/* a navigation component: subsystem 0x64, third of it */
	CHECK(svc_id_shape(0x00030064u));
	/* no subsystem below 0x20, no component 0, nothing huge */
	CHECK(!svc_id_shape(0x00020000u));
	CHECK(!svc_id_shape(0x0002001fu));
	CHECK(!svc_id_shape(0x00000027u));
	CHECK(!svc_id_shape(0x01000027u));
	CHECK(!svc_id_shape(0xffffffffu));
	CHECK(!svc_id_shape(0u));
}

static void test_collect(void)
{
	static const u32 words[] = {
		0u, 0x00020027u, 0x41414141u, 0x00010027u, 0x00020027u, 0x00030064u,
	};
	u32 ids[SVC_MAX_IDS];
	unsigned n;

	memset(ids, 0, sizeof(ids));
	n = svc_ids_collect((const u8 *)words, sizeof(words), ids, SVC_MAX_IDS, 0);
	CHECK_EQ(n, 3);                       /* the duplicate is dropped */
	CHECK_EQ(ids[0], SVC_ID_AUDIO_DEVCTRL);
	CHECK_EQ(ids[1], SVC_ID_AUDIO_PRESCTRL);
	CHECK_EQ(ids[2], 0x00030064u);

	/* a second call adds to what is there, still without duplicates */
	n = svc_ids_collect((const u8 *)words, sizeof(words), ids, SVC_MAX_IDS, n);
	CHECK_EQ(n, 3);
}

static void test_collect_full(void)
{
	u32 words[SVC_MAX_IDS + 4], ids[SVC_MAX_IDS];
	unsigned i, n;

	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		words[i] = ((i + 1) << 16) | 0x30u;     /* all of them ids */
	n = svc_ids_collect((const u8 *)words, sizeof(words), ids, SVC_MAX_IDS, 0);
	CHECK_EQ(n, SVC_MAX_IDS);             /* stops at max, no overrun */
}

static void test_unaligned_tail(void)
{
	u8 buf[7];
	u32 ids[SVC_MAX_IDS];
	u32 id = SVC_ID_AUDIO_DEVCTRL;

	memset(buf, 0, sizeof(buf));
	memcpy(buf, &id, sizeof(id));
	/* the 3 bytes left over at the end are not read as a word */
	CHECK_EQ(svc_ids_collect(buf, sizeof(buf), ids, SVC_MAX_IDS, 0), 1);
	CHECK_EQ(ids[0], SVC_ID_AUDIO_DEVCTRL);
}

static void test_rank(void)
{
	u32 ids[4] = { 0x00030064u, 0x00050027u, SVC_ID_AUDIO_PRESCTRL,
	               SVC_ID_AUDIO_DEVCTRL };

	svc_ids_rank(ids, 4);
	CHECK_EQ(ids[0], SVC_ID_AUDIO_DEVCTRL);       /* what the table says */
	CHECK_EQ(ids[1], SVC_ID_AUDIO_PRESCTRL);
	CHECK_EQ(ids[2], 0x00050027u);                /* the audio subsystem */
	CHECK_EQ(ids[3], 0x00030064u);                /* another subsystem */
}

int main(void)
{
	test_shape();
	test_collect();
	test_collect_full();
	test_unaligned_tail();
	test_rank();
	CHECK_DONE();
}

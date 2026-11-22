/*
 * test_lmstate.c - the layer manager state dump parser (src/common/lmstate.c),
 * make test. The dump is built from layermanagerV2's own format strings
 * (P1001 binary); its line order and indentation on the car are not known
 * yet, so the parser is checked against two layouts.
 */
#include "types.h"

#include "check.h"
#include "lmstate.h"

static const char dump_a[] =
	"Displayables\n"
	"Displayable id: 16\n"
	"Visible on layer with gf index 7 of display 0\n"
	"Front surface uid: 3, sid: 21\n"
	"Back surface uid: 4, sid: 22\n"
	"Displayable id: 19\n"
	"Front surface uid: 5, sid: 23\n"
	"Back surface uid: 6, sid: 24\n"
	"Displays\n"
	"The displays are not locked\n"
	"Layer 0 -> gf index: 7, state: attached to displayable: 16 \n"
	"Layer 1 -> gf index: 6, state: not attached \n"
	"Layer 2 -> gf index: 5, state: attached to displayable: 19 \n";

/* Indented, layers first, no trailing newline, a displayable without surfaces. */
static const char dump_b[] =
	"Displays\n"
	"\tLayer 1 -> gf index: 6, state: not attached \n"
	"\tLayer 0 -> gf index: 7, state: attached to displayable: 40 \n"
	"Displayables\n"
	"  Displayable id: 40\n"
	"  Back surface uid: 9, sid: 31\n"
	"  Front surface uid: 8, sid: 30\n"
	"  Displayable id: 41";

int main(void)
{
	struct lm_state_layer l[4];
	struct lm_state_displayable d[4];

	CHECK_EQ(lm_state_layers(dump_a, l, 4), 3);
	CHECK_EQ(l[0].layer, 0);
	CHECK_EQ(l[0].gf_index, 7);
	CHECK_EQ(l[0].cid, 16);
	CHECK_EQ(l[1].cid, -1);
	CHECK_EQ(l[2].gf_index, 5);
	CHECK_EQ(l[2].cid, 19);
	CHECK_EQ(lm_state_layer_cid(dump_a, 0), 16);
	CHECK_EQ(lm_state_layer_cid(dump_a, 1), -1);
	CHECK_EQ(lm_state_layer_cid(dump_a, 5), -1);
	CHECK_EQ(lm_state_unlocked(dump_a), 1);

	CHECK_EQ(lm_state_displayables(dump_a, d, 4), 2);
	CHECK_EQ(d[0].cid, 16);
	CHECK_EQ(d[0].visible_gf_index, 7);
	CHECK_EQ(d[0].visible_display, 0);
	CHECK_EQ(d[0].front_uid, 3);
	CHECK_EQ(d[0].front_sid, 21);
	CHECK_EQ(d[0].back_sid, 22);
	CHECK_EQ(d[1].cid, 19);
	CHECK_EQ(d[1].visible_gf_index, -1);
	CHECK_EQ(d[1].front_sid, 23);

	/* Count beyond max, store only max. */
	CHECK_EQ(lm_state_layers(dump_a, l, 1), 3);
	CHECK_EQ(l[0].cid, 16);
	CHECK_EQ(lm_state_displayables(dump_a, d, 1), 2);
	CHECK_EQ(d[0].cid, 16);

	CHECK_EQ(lm_state_layers(dump_b, l, 4), 2);
	CHECK_EQ(l[0].layer, 1);
	CHECK_EQ(lm_state_layer_cid(dump_b, 0), 40);
	CHECK_EQ(lm_state_unlocked(dump_b), 0);
	CHECK_EQ(lm_state_displayables(dump_b, d, 4), 2);
	CHECK_EQ(d[0].cid, 40);
	CHECK_EQ(d[0].front_sid, 30);
	CHECK_EQ(d[0].back_sid, 31);
	CHECK_EQ(d[1].cid, 41);
	CHECK_EQ(d[1].front_sid, -1);

	CHECK_EQ(lm_state_layers("", l, 4), 0);
	CHECK_EQ(lm_state_displayables("Layer x\nnothing\n", d, 4), 0);
	CHECK_EQ(lm_state_layer_cid("Layer 0 -> gf index: 7, state: bogus\n", 0), -1);

	CHECK_DONE();
}

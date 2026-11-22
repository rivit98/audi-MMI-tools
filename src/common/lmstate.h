/*
 * lmstate.h - parse the layer manager's state dump (pure, tests/test_lmstate.c).
 *
 * A read() of /dev/layermanager returns layermanagerV2's
 * CGraphicsManager::dumpCurrentState text (lm_read_state() in lmgr.h).
 * The lines parsed here, format strings as in the P1001 binary:
 *
 *   Displayable id: %d
 *   Visible on layer with gf index %d of display %d
 *   Front surface uid: %d, sid: %d
 *   Back surface uid: %d, sid: %d
 *   Layer %d -> gf index: %d, state: attached to displayable: %d
 *   Layer %d -> gf index: %d, state: not attached
 *   The displays are not locked
 *
 * Every function takes the dump as one NUL-terminated string.
 */
#ifndef COMMON_LMSTATE_H
#define COMMON_LMSTATE_H

struct lm_state_layer {
	int layer;      /* "Layer %d" */
	int gf_index;   /* "gf index: %d" */
	int cid;        /* attached displayable, -1 = not attached */
};

struct lm_state_displayable {
	int cid;
	int front_uid, front_sid;   /* -1 if the dump has no such line */
	int back_uid, back_sid;
	int visible_gf_index;       /* "Visible on layer with gf index", -1 = none */
	int visible_display;
};

/* The Layer lines, in dump order (one block per display). Returns how many
 * were found, at most max are stored. */
int lm_state_layers(const char *text, struct lm_state_layer *out, int max);

/* The Displayable id blocks, in dump order. Returns how many were found,
 * at most max are stored. */
int lm_state_displayables(const char *text, struct lm_state_displayable *out, int max);

/* The displayable attached to the first "Layer <layer>" line (the HMI's
 * foreground is on layer 0), or -1. */
int lm_state_layer_cid(const char *text, int layer);

/* 1 if the dump says "The displays are not locked", 0 otherwise. */
int lm_state_unlocked(const char *text);

#endif

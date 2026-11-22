#include "lmstate.h"

#include <stdio.h>
#include <string.h>

/* Next line after p, or NULL at the end. */
static const char *next_line(const char *p)
{
	const char *nl = strchr(p, '\n');

	return nl ? nl + 1 : NULL;
}

/* p with leading blanks skipped (the dump may indent its lines). */
static const char *skip_blanks(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

static int parse_layer(const char *p, struct lm_state_layer *l)
{
	int layer, gf, cid, n = 0;

	if (sscanf(p, "Layer %d -> gf index: %d, state: %n", &layer, &gf, &n) < 2 || n == 0)
		return 0;
	p += n;
	if (sscanf(p, "attached to displayable: %d", &cid) == 1)
		;
	else if (strncmp(p, "not attached", 12) == 0)
		cid = -1;
	else
		return 0;
	l->layer = layer;
	l->gf_index = gf;
	l->cid = cid;
	return 1;
}

int lm_state_layers(const char *text, struct lm_state_layer *out, int max)
{
	const char *p;
	int count = 0;

	for (p = text; p != NULL && *p; p = next_line(p)) {
		struct lm_state_layer l;

		if (!parse_layer(skip_blanks(p), &l))
			continue;
		if (count < max)
			out[count] = l;
		count++;
	}
	return count;
}

int lm_state_displayables(const char *text, struct lm_state_displayable *out, int max)
{
	struct lm_state_displayable cur;
	const char *p;
	int count = 0, open = 0;

	for (p = text; p != NULL && *p; p = next_line(p)) {
		const char *q = skip_blanks(p);
		int a, b;

		if (sscanf(q, "Displayable id: %d", &a) == 1) {
			if (open && count < max)
				out[count] = cur;
			count += open;
			memset(&cur, 0, sizeof(cur));
			cur.cid = a;
			cur.front_uid = cur.front_sid = cur.back_uid = cur.back_sid = -1;
			cur.visible_gf_index = cur.visible_display = -1;
			open = 1;
		} else if (!open) {
			continue;   /* a block runs until the next "Displayable id" */
		} else if (sscanf(q, "Front surface uid: %d, sid: %d", &a, &b) == 2) {
			cur.front_uid = a;
			cur.front_sid = b;
		} else if (sscanf(q, "Back surface uid: %d, sid: %d", &a, &b) == 2) {
			cur.back_uid = a;
			cur.back_sid = b;
		} else if (sscanf(q, "Visible on layer with gf index %d of display %d", &a, &b) == 2) {
			cur.visible_gf_index = a;
			cur.visible_display = b;
		}
	}
	if (open) {
		if (count < max)
			out[count] = cur;
		count++;
	}
	return count;
}

int lm_state_layer_cid(const char *text, int layer)
{
	const char *p;

	for (p = text; p != NULL && *p; p = next_line(p)) {
		struct lm_state_layer l;

		if (parse_layer(skip_blanks(p), &l) && l.layer == layer)
			return l.cid;
	}
	return -1;
}

int lm_state_unlocked(const char *text)
{
	return strstr(text, "The displays are not locked") != NULL;
}

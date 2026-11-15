/*
 * svcid.c - see svcid.h.
 */
#include <string.h>

#include "svcid.h"

int svc_id_shape(u32 v)
{
	u32 subsys = v & 0xffffu;
	u32 instance = v >> 16;

	return subsys >= SVC_SUBSYS_MIN && subsys <= SVC_SUBSYS_MAX &&
	       instance >= 1 && instance <= SVC_INSTANCE_MAX;
}

unsigned svc_ids_collect(const u8 *buf, usize len, u32 *ids, unsigned max,
                         unsigned n)
{
	usize off;

	for (off = 0; off + sizeof(u32) <= len; off += sizeof(u32)) {
		u32 v;
		unsigned i;

		memcpy(&v, buf + off, sizeof(v));
		if (!svc_id_shape(v))
			continue;
		for (i = 0; i < n; i++)
			if (ids[i] == v)
				break;
		if (i < n || n >= max)
			continue;
		ids[n++] = v;
	}
	return n;
}

/* How good a guess an id is: lower is better. */
static unsigned rank_of(u32 v)
{
	if (v == SVC_ID_AUDIO_DEVCTRL)
		return 0;
	if (v == SVC_ID_AUDIO_PRESCTRL)
		return 1;
	if ((v & 0xffffu) == (SVC_ID_AUDIO_DEVCTRL & 0xffffu))
		return 2;
	return 3;
}

void svc_ids_rank(u32 *ids, unsigned n)
{
	unsigned i, j;

	for (i = 1; i < n; i++) {         /* insertion sort: n is tiny */
		u32 v = ids[i];
		unsigned r = rank_of(v);

		for (j = i; j > 0 && rank_of(ids[j - 1]) > r; j--)
			ids[j] = ids[j - 1];
		ids[j] = v;
	}
}

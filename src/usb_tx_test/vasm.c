/*
 * vasm.c - video picture reassembly (see vasm.h).
 */
#include <stdlib.h>
#include <string.h>

#include "proto.h"
#include "vasm.h"

void vasm_init(struct vasm *a, uint32_t max_bytes)
{
	memset(a, 0, sizeof(*a));
	a->max_bytes = max_bytes;
}

void vasm_free(struct vasm *a)
{
	free(a->cur.data);
	a->cur.data = NULL;
	a->active = 0;
	a->have = 0;
}

int vasm_start(struct vasm *a, const uint8_t *p, unsigned n, int *dropped_id,
               int *bad_id)
{
	struct vpic pic;

	*dropped_id = a->active ? (int)a->cur.id : -1;
	vasm_free(a);
	if (n < PROTO_VIDEO_START_LEN) {
		*bad_id = n >= 2 ? proto_get_u16(p) : 0;
		return VASM_BAD;
	}
	pic.id = proto_get_u16(p);
	pic.w = proto_get_u16(p + 2);
	pic.h = proto_get_u16(p + 4);
	pic.format = p[6];
	pic.size = proto_get_u32(p + 7);
	*bad_id = (int)pic.id;
	if (pic.size == 0 || pic.size > a->max_bytes || pic.format != PROTO_FMT_JPEG ||
	    pic.w == 0 || pic.h == 0)
		return VASM_BAD;
	pic.data = malloc(pic.size);
	if (pic.data == NULL)
		return VASM_BAD;
	a->cur = pic;
	a->have = 0;
	a->active = 1;
	return VASM_OK;
}

int vasm_data(struct vasm *a, const uint8_t *p, unsigned n, int *bad_id)
{
	unsigned len;

	if (n < 2 || !a->active || proto_get_u16(p) != a->cur.id)
		return VASM_IGNORED;
	len = n - 2;
	if (len > a->cur.size - a->have) {
		*bad_id = (int)a->cur.id;
		vasm_free(a);
		return VASM_BAD;
	}
	memcpy(a->cur.data + a->have, p + 2, len);
	a->have += len;
	return a->have == a->cur.size ? VASM_DONE : VASM_OK;
}

void vasm_take(struct vasm *a, struct vpic *out)
{
	*out = a->cur;
	a->cur.data = NULL;
	a->active = 0;
	a->have = 0;
}

#include "swdec.h"

#include <stdlib.h>
#include <string.h>

#include "h264bsd_decoder.h"

/* An access unit delimiter: the decoder's end-of-picture signal. */
static const u8 kAud[] = { 0, 0, 0, 1, 0x09, 0xf0 };

struct swdec {
	storage_t *s;
	u8 *buf;            /* the decoder writes into its input: our copy */
	usize cap;
	u32 pic_id;
	int ready;          /* h264bsdInit done: shut down before free */
};

struct swdec *swdec_open(void)
{
	struct swdec *d = calloc(1, sizeof(*d));

	if (d == NULL)
		return NULL;
	d->s = h264bsdAlloc();
	/* noOutputReordering: Baseline has no B frames, output at once */
	if (d->s == NULL || h264bsdInit(d->s, 1) != H264BSD_RDY) {
		swdec_close(d);
		return NULL;
	}
	d->ready = 1;
	return d;
}

void swdec_close(struct swdec *d)
{
	if (d == NULL)
		return;
	if (d->s != NULL) {
		if (d->ready)
			h264bsdShutdown(d->s);
		h264bsdFree(d->s);   /* free() only */
	}
	free(d->buf);
	free(d);
}

static void fill(struct swdec *d, const u8 *y, u32 idr, u32 err, struct swdec_pic *pic)
{
	u32 crop = 0, left = 0, w = 0, top = 0, h = 0;

	pic->i420 = y;
	pic->width = (int)h264bsdPicWidth(d->s) * 16;
	pic->height = (int)h264bsdPicHeight(d->s) * 16;
	h264bsdCroppingParams(d->s, &crop, &left, &w, &top, &h);
	pic->crop_left = crop ? (int)left : 0;
	pic->crop_top = crop ? (int)top : 0;
	pic->crop_w = crop ? (int)w : pic->width;
	pic->crop_h = crop ? (int)h : pic->height;
	pic->idr = idr != 0;
	pic->error_mbs = (int)err;
}

int swdec_decode(struct swdec *d, const u8 *au, usize n, struct swdec_pic *pic)
{
	usize total = n + sizeof(kAud);
	u8 *p;
	u32 left, got = 0;
	int stalls = 0;

	if (total > d->cap) {
		u8 *b = realloc(d->buf, total);

		if (b == NULL)
			return -1;
		d->buf = b;
		d->cap = total;
	}
	memcpy(d->buf, au, n);
	memcpy(d->buf + n, kAud, sizeof(kAud));
	p = d->buf;
	left = (u32)total;
	while (left > 0) {
		u32 read = 0, rc = h264bsdDecode(d->s, p, left, d->pic_id, &read);

		if (read > left)
			return -1;
		p += read;
		left -= read;
		if (rc == H264BSD_PIC_RDY) {
			u32 id, idr, err;
			const u8 *y = h264bsdNextOutputPicture(d->s, &id, &idr, &err);

			if (y != NULL) {
				fill(d, y, idr, err, pic);
				got = 1;
			}
			d->pic_id++;
		} else if (rc == H264BSD_PARAM_SET_ERROR || rc == H264BSD_MEMALLOC_ERROR) {
			return -1;
		}
		/* H264BSD_ERROR: a broken slice, concealed; go on */
		if (read == 0 && rc != H264BSD_PIC_RDY && ++stalls > 2)
			break;
	}
	return (int)got;
}

static u8 clip8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (u8)v;
}

void swdec_to_bgra(const struct swdec_pic *pic, u32 *out, int stride)
{
	const int cw = pic->width / 2;
	const u8 *Y = pic->i420;
	const u8 *U = Y + (usize)pic->width * pic->height;
	const u8 *V = U + (usize)cw * (pic->height / 2);
	int x, y;

	for (y = 0; y < pic->crop_h; y++) {
		int sy = pic->crop_top + y;
		const u8 *yr = Y + (usize)sy * pic->width + pic->crop_left;
		const u8 *ur = U + (usize)(sy / 2) * cw;
		const u8 *vr = V + (usize)(sy / 2) * cw;
		u32 *o = out + (usize)y * stride;

		for (x = 0; x < pic->crop_w; x++) {
			int sx = (pic->crop_left + x) / 2;
			int c = 298 * (yr[x] - 16) + 128;
			int d = ur[sx] - 128, e = vr[sx] - 128;

			o[x] = 0xff000000u | (u32)clip8((c + 409 * e) >> 8) << 16 |
			       (u32)clip8((c - 100 * d - 208 * e) >> 8) << 8 |
			       (u32)clip8((c + 516 * d) >> 8);
		}
	}
}

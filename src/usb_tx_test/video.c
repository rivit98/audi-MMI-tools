/*
 * video.c - picture hand-over, decode and acks (see video.h).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg.h"
#include "log.h"
#include "video.h"

#define ACK_QUEUE   8
#define FPS_WINDOW  10000      /* ms */
#define LOG_FIRST   5          /* pictures logged one by one, then every LOG_EVERY */
#define LOG_EVERY   20

static int land_w = 800, land_h = 480 - VIDEO_TOPBAR_H, port_w = 400, port_h = 480;

/* --- shared with the usbd and manager threads (lock) ------------------- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct vpic     waiting;          /* data == NULL: none */
static int             stop_pending;
static unsigned        dropped;          /* replaced while waiting */
static char            stop_why[64];
static uint8_t         acks[ACK_QUEUE][PROTO_VIDEO_ACK_LEN];
static unsigned        ack_head, ack_n;
static void          (*notify)(void);

/* --- main loop only ----------------------------------------------------- */
static struct video_view view;
static int               have_view;
static int               unacked;        /* view.id not acked yet */
static unsigned          shown_at[64];   /* log_ms() of recent pictures, a ring */
static unsigned          shown_n;

void video_init(int screen_w, int screen_h)
{
	land_w = screen_w;
	land_h = screen_h - VIDEO_TOPBAR_H;
	port_w = screen_w / 2;
	port_h = screen_h;
	log_msg("video: boxes landscape %dx%d, portrait %dx%d, max %u bytes",
	        land_w, land_h, port_w, port_h, VIDEO_MAX_BYTES);
}

void video_box(int portrait, int *w, int *h)
{
	*w = portrait ? port_w : land_w;
	*h = portrait ? port_h : land_h;
}

void video_caps(uint8_t *out)
{
	proto_put_u16(out, (unsigned)land_w);
	proto_put_u16(out + 2, (unsigned)land_h);
	proto_put_u16(out + 4, (unsigned)port_w);
	proto_put_u16(out + 6, (unsigned)port_h);
	proto_put_u32(out + 8, VIDEO_MAX_BYTES);
	out[12] = 1u << (PROTO_FMT_JPEG - 1);
}

void video_set_notify(void (*fn)(void))
{
	notify = fn;
}

void video_ack(unsigned id, int status, unsigned decode_ms)
{
	uint8_t *a;

	pthread_mutex_lock(&lock);
	if (ack_n == ACK_QUEUE) {                /* nobody sends them: keep the newest */
		ack_head = (ack_head + 1) % ACK_QUEUE;
		ack_n--;
	}
	a = acks[(ack_head + ack_n) % ACK_QUEUE];
	proto_put_u16(a, id);
	a[2] = (uint8_t)status;
	proto_put_u16(a + 3, decode_ms > 0xffff ? 0xffff : decode_ms);
	ack_n++;
	pthread_mutex_unlock(&lock);
	if (notify != NULL)
		notify();
}

int video_ack_pop(uint8_t *out)
{
	int got = 0;

	pthread_mutex_lock(&lock);
	if (ack_n > 0) {
		memcpy(out, acks[ack_head], PROTO_VIDEO_ACK_LEN);
		ack_head = (ack_head + 1) % ACK_QUEUE;
		ack_n--;
		got = 1;
	}
	pthread_mutex_unlock(&lock);
	return got;
}

void video_submit(struct vpic *pic)
{
	struct vpic old;

	pthread_mutex_lock(&lock);
	old = waiting;
	waiting = *pic;
	stop_pending = 0;                        /* a picture after a stop: streaming again */
	pthread_mutex_unlock(&lock);
	pic->data = NULL;
	if (old.data != NULL) {
		pthread_mutex_lock(&lock);
		dropped++;
		pthread_mutex_unlock(&lock);
		free(old.data);
		video_ack(old.id, PROTO_VIDEO_DROPPED, 0);
	}
}

void video_stop(const char *why)
{
	struct vpic old;

	pthread_mutex_lock(&lock);
	old = waiting;
	waiting.data = NULL;
	stop_pending = 1;
	snprintf(stop_why, sizeof(stop_why), "%s", why);
	pthread_mutex_unlock(&lock);
	free(old.data);
}

static unsigned fps_x10(unsigned now)
{
	unsigned i, n = 0, ring = sizeof(shown_at) / sizeof(shown_at[0]);
	unsigned count = shown_n < ring ? shown_n : ring;

	for (i = 0; i < count; i++)
		if (now - shown_at[i] < FPS_WINDOW)
			n++;
	return n * 10000 / FPS_WINDOW;
}

static void drop_view(void)
{
	free(view.frame.px);
	view.frame.px = NULL;
	have_view = 0;
}

int video_poll(void)
{
	struct vpic pic;
	char why[64];
	uint32_t *px;
	unsigned t0, ms;
	int stop, w, h, max_w, max_h, portrait;

	pthread_mutex_lock(&lock);
	stop = stop_pending;
	stop_pending = 0;
	memcpy(why, stop_why, sizeof(why));
	pic = waiting;
	waiting.data = NULL;
	view.dropped = dropped;
	if (stop)
		dropped = 0;
	pthread_mutex_unlock(&lock);

	if (stop) {
		int had = have_view;

		if (have_view)
			log_msg("video: stopped (%s) after %u shown, %u bad, %u dropped",
			        why, view.shown, view.bad, view.dropped);
		if (unacked)
			video_ack(view.id, PROTO_VIDEO_DROPPED, view.decode_ms);
		unacked = 0;
		drop_view();
		memset(&view, 0, sizeof(view));
		shown_n = 0;
		if (pic.data == NULL)
			return had;
	}
	if (pic.data == NULL)
		return 0;

	portrait = pic.w < pic.h;
	video_box(portrait, &max_w, &max_h);
	t0 = log_ms();
	if (jpeg_decode_argb(pic.data, pic.size, max_w, max_h, &px, &w, &h) != 0) {
		jpeg_info(pic.data, pic.size, &w, &h);
		log_msg("video: picture %u (%u bytes, %ux%u) not shown: %s", pic.id, pic.size,
		        pic.w, pic.h, w > max_w || h > max_h ? "larger than its box" : "bad JPEG");
		free(pic.data);
		view.bad++;
		video_ack(pic.id, PROTO_VIDEO_BAD, 0);
		return 0;
	}
	ms = log_ms() - t0;
	free(pic.data);

	if (unacked)                             /* never presented (headless) */
		video_ack(view.id, PROTO_VIDEO_DROPPED, view.decode_ms);
	free(view.frame.px);
	view.frame.px = px;
	view.frame.w = w;
	view.frame.h = h;
	view.frame.stride = w;
	view.portrait = w < h;
	view.id = pic.id;
	view.bytes = pic.size;
	view.decode_ms = ms;
	have_view = 1;
	unacked = 1;
	if ((unsigned)w != pic.w || (unsigned)h != pic.h)
		log_msg("video: picture %u announced %ux%u, is %dx%d", pic.id, pic.w, pic.h, w, h);
	return 1;
}

void video_presented(unsigned present_ms)
{
	unsigned now = log_ms(), ring = sizeof(shown_at) / sizeof(shown_at[0]);

	if (!unacked)
		return;
	unacked = 0;
	view.present_ms = present_ms;
	view.shown++;
	shown_at[shown_n++ % ring] = now;
	view.fps_x10 = fps_x10(now);
	video_ack(view.id, PROTO_VIDEO_SHOWN, view.decode_ms);
	if (view.shown <= LOG_FIRST || view.shown % LOG_EVERY == 0)
		log_msg("video: #%u id %u %dx%d %s, %u bytes, decode %u ms, present %u ms, "
		        "%u.%u fps", view.shown, view.id, view.frame.w, view.frame.h,
		        view.portrait ? "portrait" : "landscape", view.bytes, view.decode_ms,
		        present_ms, view.fps_x10 / 10, view.fps_x10 % 10);
}

const struct video_view *video_view(void)
{
	return have_view ? &view : NULL;
}

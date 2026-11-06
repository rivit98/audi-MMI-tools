/*
 * video.c - picture hand-over, decode and acks (see video.h).
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hwvideo.h"
#include "imgdec.h"
#include "log.h"
#include "perf.h"
#include "video.h"

#define ACK_QUEUE   8
#define FPS_WINDOW  10000      /* ms */
#define LOG_FIRST   3          /* pictures logged one by one; then perf.c's summaries */

static int land_w = 800, land_h = 480, port_w = 400, port_h = 480;

/* --- shared with the usbd and manager threads (lock) ------------------- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  work;             /* a picture or a stop is waiting */
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
static struct perf_pic   rec;            /* view's timings so far */

/* A picture that never reached the screen. */
static void perf_lost(const struct vpic *pic, int status)
{
	struct perf_pic r;

	memset(&r, 0, sizeof(r));
	r.t_ms = log_ms();
	r.id = pic->id;
	r.bytes = pic->size;
	r.w = pic->w;
	r.h = pic->h;
	r.format = pic->format;
	r.phone_ms = pic->phone_ms;
	r.status = status;
	r.rx_us = pic->rx_done_us - pic->rx_start_us;
	perf_add(&r);
}

void video_init(int screen_w, int screen_h)
{
	pthread_condattr_t ca;

	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&work, &ca);
	pthread_condattr_destroy(&ca);
	land_w = screen_w;
	land_h = screen_h;                       /* the status bar goes over it */
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
	out[12] = 1u << (PROTO_FMT_JPEG - 1) | 1u << (PROTO_FMT_PNG - 1) |
	          1u << (PROTO_FMT_H264 - 1);
	proto_put_u16(out + 13, HWVIDEO_MAX_W);
	proto_put_u16(out + 15, HWVIDEO_MAX_H);
	out[17] = HWVIDEO_FPS;
	proto_put_u32(out + 18, HWVIDEO_MAX_FRAME);
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
	pthread_cond_signal(&work);
	pic->data = NULL;
	if (old.data != NULL) {
		pthread_mutex_lock(&lock);
		dropped++;
		pthread_mutex_unlock(&lock);
		free(old.data);
		video_ack(old.id, PROTO_VIDEO_DROPPED, 0);
		perf_lost(&old, PROTO_VIDEO_DROPPED);
	}
}

void video_wait(unsigned ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&lock);
	while (waiting.data == NULL && !stop_pending)
		if (pthread_cond_timedwait(&work, &lock, &ts) == ETIMEDOUT)
			break;
	pthread_mutex_unlock(&lock);
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
	pthread_cond_signal(&work);
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
	unsigned t0, us;
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
		if (unacked) {
			video_ack(view.id, PROTO_VIDEO_DROPPED, view.decode_ms);
			rec.status = PROTO_VIDEO_DROPPED;
			rec.t_ms = log_ms();
			perf_add(&rec);
		}
		unacked = 0;
		drop_view();
		memset(&view, 0, sizeof(view));
		shown_n = 0;
		perf_flush();
		if (pic.data == NULL)
			return had;
	}
	if (pic.data == NULL)
		return 0;

	portrait = pic.w < pic.h;
	video_box(portrait, &max_w, &max_h);
	t0 = log_us();
	if (imgdec_argb(pic.data, pic.size, max_w, max_h, &px, &w, &h) != 0) {
		imgdec_info(pic.data, pic.size, &w, &h);
		log_msg("video: picture %u (%s, %u bytes, %ux%u) not shown: %s", pic.id,
		        proto_fmt_name(pic.format), pic.size, pic.w, pic.h,
		        w > max_w || h > max_h ? "larger than its box" : "not decodable");
		free(pic.data);
		view.bad++;
		video_ack(pic.id, PROTO_VIDEO_BAD, 0);
		perf_lost(&pic, PROTO_VIDEO_BAD);
		return 0;
	}
	us = log_us() - t0;
	free(pic.data);

	if (unacked) {                           /* never presented (headless) */
		video_ack(view.id, PROTO_VIDEO_DROPPED, view.decode_ms);
		rec.status = PROTO_VIDEO_DROPPED;
		rec.t_ms = log_ms();
		perf_add(&rec);
	}
	memset(&rec, 0, sizeof(rec));
	rec.id = pic.id;
	rec.bytes = pic.size;
	rec.w = (unsigned)w;
	rec.h = (unsigned)h;
	rec.format = pic.format;
	rec.phone_ms = pic.phone_ms;
	rec.rx_us = pic.rx_done_us - pic.rx_start_us;
	rec.wait_us = t0 - pic.rx_done_us;
	rec.decode_us = us;
	rec.latency_us = pic.rx_start_us;        /* start; the span when shown */
	free(view.frame.px);
	view.frame.px = px;
	view.frame.w = w;
	view.frame.h = h;
	view.frame.stride = w;
	view.portrait = w < h;
	view.id = pic.id;
	view.bytes = pic.size;
	view.format = pic.format;
	view.decode_ms = (us + 500) / 1000;
	have_view = 1;
	unacked = 1;
	if ((unsigned)w != pic.w || (unsigned)h != pic.h)
		log_msg("video: picture %u announced %ux%u, is %dx%d", pic.id, pic.w, pic.h, w, h);
	return 1;
}

void video_presented(const struct video_present *p)
{
	unsigned now = log_ms(), ring = sizeof(shown_at) / sizeof(shown_at[0]);

	if (!unacked)
		return;
	unacked = 0;
	if (p != NULL) {
		rec.render_us = p->render_us;
		rec.copy_us = p->copy_us;
		rec.flip_us = p->flip_us;
		rec.vfb_bytes = p->vfb_bytes;
		rec.vfb_rows = p->vfb_rows;
		rec.vfb_full = p->vfb_full;
		view.present_ms = (p->render_us + p->copy_us + p->flip_us + 500) / 1000;
	} else {
		view.present_ms = 0;
	}
	view.shown++;
	shown_at[shown_n++ % ring] = now;
	view.fps_x10 = fps_x10(now);
	video_ack(view.id, PROTO_VIDEO_SHOWN, view.decode_ms);
	rec.t_ms = now;
	rec.status = PROTO_VIDEO_SHOWN;
	rec.latency_us = log_us() - rec.latency_us;
	perf_add(&rec);
	if (view.shown <= LOG_FIRST)
		log_msg("video: #%u id %u %dx%d %s, %s %u bytes (phone %u ms), decode %u ms, "
		        "present %u ms (render %u, copy %u, flip %u), vfb %u KB", view.shown,
		        view.id, view.frame.w, view.frame.h,
		        view.portrait ? "portrait" : "landscape", proto_fmt_name(view.format),
		        view.bytes, rec.phone_ms, view.decode_ms, view.present_ms, rec.render_us / 1000,
		        rec.copy_us / 1000, rec.flip_us / 1000, rec.vfb_bytes / 1024);
}

const struct video_view *video_view(void)
{
	return have_view ? &view : NULL;
}

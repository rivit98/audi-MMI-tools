/*
 * video.h - the phone's pictures, between the USB thread and the screen.
 *
 *   usbd thread    video_submit() a complete JPEG (vasm.h), video_stop()
 *   main loop      video_poll() decodes the newest waiting picture,
 *                  ui.c draws video_view(), video_presented() acks it
 *   manager        video_ack_pop() -> VIDEO_ACK frames on the link
 *
 * Only the newest picture waits: one that arrives before the previous is
 * decoded replaces it (that one is acked DROPPED). The phone keeps one
 * picture in flight, so normally nothing is dropped.
 *
 * Portrait vs landscape is the picture's own shape (w < h = portrait). The
 * MMI tells the phone which box each fits in (VIDEO_CAPS); ui.c lays the
 * screen out to match: landscape under a slim status bar, portrait on the
 * left with the text panel beside it.
 */
#ifndef USBTX_VIDEO_H
#define USBTX_VIDEO_H

#include <stdint.h>

#include "proto.h"
#include "text.h"
#include "vasm.h"

#define VIDEO_TOPBAR_H   28            /* status bar above a landscape picture */
#define VIDEO_MAX_BYTES  (512u * 1024) /* one JPEG */

struct video_view {
	struct canvas frame;     /* the picture on screen, opaque ARGB */
	int           portrait;
	unsigned      id, shown, bad, dropped;
	unsigned      decode_ms, present_ms;
	unsigned      fps_x10;   /* pictures shown per 10 s */
	unsigned      bytes;     /* size of the JPEG on screen */
};

/* Screen size: sets the boxes in VIDEO_CAPS. */
void video_init(int screen_w, int screen_h);
/* Box a landscape / portrait picture has to fit in. */
void video_box(int portrait, int *w, int *h);
/* VIDEO_CAPS payload (PROTO_VIDEO_CAPS_LEN bytes). */
void video_caps(uint8_t *out);

/* Called (any thread) when an ack is queued, to wake the sender. */
void video_set_notify(void (*fn)(void));

/* usbd thread. Takes ownership of pic->data. */
void video_submit(struct vpic *pic);
/* Streaming ended (VIDEO_STOP or link down): back to the text screen. */
void video_stop(const char *why);
/* Queue a VIDEO_ACK (any thread). */
void video_ack(unsigned id, int status, unsigned decode_ms);
/* Manager thread: next queued ack payload (PROTO_VIDEO_ACK_LEN bytes),
 * 1 if there was one. */
int video_ack_pop(uint8_t *out);

/* Main loop: handle a stop and decode the newest waiting picture.
 * 1 if what video_view() shows changed. */
int video_poll(void);
/* Main loop, after the screen with the new picture was presented
 * (present_ms = render + VFB copy). Acks it. */
void video_presented(unsigned present_ms);
/* NULL when there is no picture (no streaming). Main loop only. */
const struct video_view *video_view(void);

#endif

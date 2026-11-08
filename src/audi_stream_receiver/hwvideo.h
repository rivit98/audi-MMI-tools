/*
 * hwvideo.h - the phone's H.264 stream on the screen through the unit's
 * DM642 decoder (common/hw_stream.h; protocol: proto.h H264_CONFIG, format
 * PROTO_FMT_H264, H264_STATUS). The C side of audi_stream_receiver talks to
 * it through these calls; it runs its own thread ("hw"), which starts,
 * polls and stops the stream.
 *
 *   usbd thread   hwvideo_config() (H264_CONFIG), hwvideo_frame() (a
 *                 complete frame), hwvideo_stop() (VIDEO_STOP, link down,
 *                 a JPEG picture: the phone went back to pictures)
 *   manager       hwvideo_status_pop() -> H264_STATUS frames
 *   main loop     hwvideo_active(): while 1 the screen belongs to the
 *                 video; close the CPU-drawn screen, open it again after
 *
 * Writes "h264:" lines to the app's log: the config, the stream's start
 * and end, refused frames, and every 10 s frames in / skips / io-media's
 * reads and lag.
 */
#ifndef AUDI_STREAM_HWVIDEO_H
#define AUDI_STREAM_HWVIDEO_H

#include <stdint.h>

/* The limits announced in VIDEO_CAPS (docs/phone-mirroring.md 7.10). */
#define HWVIDEO_MAX_W      720
#define HWVIDEO_MAX_H      576
#define HWVIDEO_FPS        25
#define HWVIDEO_MAX_FRAME  (256u * 1024)

#ifdef __cplusplus
extern "C" {
#endif

void hwvideo_start(void);
void hwvideo_shutdown(void);
/* Called (any thread) when a status is queued, to wake the sender. */
void hwvideo_set_notify(void (*fn)(void));

void hwvideo_config(const uint8_t *payload, unsigned n);
/* One frame (Annex-B). Returns the VIDEO_ACK status (PROTO_VIDEO_*). */
int  hwvideo_frame(const uint8_t *data, unsigned size);
void hwvideo_stop(const char *why);

/* Manager thread: next H264_STATUS code, 1 if there was one. */
int  hwvideo_status_pop(uint8_t *code);
/* Main loop: the video has (or is about to have) the screen. */
int  hwvideo_active(void);

#ifdef __cplusplus
}
#endif

#endif

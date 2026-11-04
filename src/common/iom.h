/*
 * iom.h - a minimal io-media client: play files on a mixer output without
 * spawning the unit's testapp-file.
 *
 * No library: testapp-file (efs-system bin/, NEEDED only libc.so.2) drives
 * io-media with open/devctl/read on a graph file under
 * /dev/io-media/graphs/, and this does the same. The devctl codes and
 * payloads are read from its disassembly (P1001 efs-system), not from any
 * header:
 *   create   open(".../trackplayer/<name>", O_RDWR|O_CREAT|O_EXCL, 0626),
 *            FD_CLOEXEC, devctl 0xC0040F00 {int 1}         sub_8041500
 *   speed    devctl 0x80040F0B {int 1000}                   sub_8041B60
 *   time     devctl 0x80040F0C {int ms}: TIME event period  sub_8041FC0
 *   output   devctlv 0x80080F20 {int 1}{url\0} -> output id
 *            in dev_info                                    sub_80417C0
 *   volume   devctlv 0x80080F22 {int output}{u8 volume,
 *            balance 50, fade 50, 0}                        sub_8041BE0
 *   input    devctlv 0x80040F01 {u64 track}{url\0}          sub_80418C0
 *   detach   devctl 0xF02 (the input, after INPUT_EOF)      sub_8041920
 *   play     devctl 0xF06                                   sub_8041AE0
 *   errinfo  devctl 0x40100F11 -> {u32 iom_errno, u32 type,
 *            u64 value}                                     sub_8041780
 *   events   _IO_READ with xtype _IO_XFLAG_BLOCK (0x8000);
 *            here _IO_XFLAG_NONBLOCK (0x4000), EAGAIN = none sub_8042CE0
 * Closing the fd destroys the graph. devctl returns EOK or an errno
 * value; so do these.
 */
#ifndef COMMON_IOM_H
#define COMMON_IOM_H

#include <stdint.h>

/* Event types (testapp-file's switch at 0x8040FC8). Each event is a
 * {u32 size, u32 type} header, size counting the header, then the
 * payload. */
enum {
	IOM_EV_INPUT_EOF = 1,   /* u64 track: the input was read to its end */
	IOM_EV_TIME      = 2,   /* u64 track, s64 cur ms, u64 total ms */
	IOM_EV_TRACK     = 3,
	IOM_EV_STATUS    = 4,   /* u64 track, u32 state (4 = playing), s32 speed */
	IOM_EV_FINISHED  = 5,
	IOM_EV_ERROR     = 8,
};

struct iom {
	int  fd;
	int  output;            /* id from iom_attach_output */
	char path[128];
};

struct iom_err {
	uint32_t iom_errno;
	uint32_t value_type;
	uint64_t value;
};

int  iom_create(struct iom *g, const char *name);
int  iom_set_speed(struct iom *g, int speed);          /* 1000 = normal */
int  iom_set_time_notify(struct iom *g, int ms);
int  iom_attach_output(struct iom *g, const char *url);
int  iom_set_volume(struct iom *g, int volume);         /* 0..100 */
int  iom_attach_input(struct iom *g, uint64_t track, const char *url);
int  iom_detach_input(struct iom *g);
int  iom_play(struct iom *g);
int  iom_error_info(struct iom *g, struct iom_err *e);

/* Events waiting, without blocking: bytes read, 0 if none, -1 (errno). */
int  iom_read_events(struct iom *g, void *buf, int len);

void iom_close(struct iom *g);

#endif

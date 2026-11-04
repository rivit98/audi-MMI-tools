/*
 * iom.c - see iom.h.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/uio.h>

#include "iom.h"

/* <sys/iomsg.h> has it, but needs a <sys/pm.h> the sysroot lacks */
#define IO_XFLAG_NONBLOCK 0x4000

#define DCMD_CREATE       0xC0040F00
#define DCMD_SPEED        0x80040F0B
#define DCMD_TIME_NOTIFY  0x80040F0C
#define DCMD_OUTPUT       0x80080F20
#define DCMD_VOLUME       0x80080F22
#define DCMD_INPUT        0x80040F01
#define DCMD_DETACH_INPUT 0x00000F02
#define DCMD_PLAY         0x00000F06
#define DCMD_ERROR_INFO   0x40100F11

static int dc_int(struct iom *g, int dcmd, int value)
{
	return devctl(g->fd, dcmd, &value, sizeof(value), NULL);
}

int iom_create(struct iom *g, const char *name)
{
	int flags, rc;

	g->output = -1;
	snprintf(g->path, sizeof(g->path), "/dev/io-media/graphs/trackplayer/%s", name);
	g->fd = open(g->path, O_RDWR | O_CREAT | O_EXCL, 0626);
	if (g->fd < 0)
		return errno;
	flags = fcntl(g->fd, F_GETFD);
	if (flags >= 0)
		fcntl(g->fd, F_SETFD, flags | FD_CLOEXEC);
	rc = dc_int(g, DCMD_CREATE, 1);
	if (rc != EOK) {
		unlink(g->path);
		close(g->fd);
		g->fd = -1;
	}
	return rc;
}

int iom_set_speed(struct iom *g, int speed)
{
	return dc_int(g, DCMD_SPEED, speed);
}

int iom_set_time_notify(struct iom *g, int ms)
{
	return dc_int(g, DCMD_TIME_NOTIFY, ms);
}

int iom_attach_output(struct iom *g, const char *url)
{
	struct iovec iov[2];
	int one = 1, id = -1, rc;

	SETIOV(&iov[0], &one, sizeof(one));
	SETIOV(&iov[1], url, strlen(url) + 1);
	rc = devctlv(g->fd, DCMD_OUTPUT, 2, 0, iov, NULL, &id);
	if (rc == EOK)
		g->output = id;
	return rc;
}

int iom_set_volume(struct iom *g, int volume)
{
	unsigned char v[4] = { (unsigned char)volume, 50, 50, 0 };
	struct iovec iov[2];

	SETIOV(&iov[0], &g->output, sizeof(g->output));
	SETIOV(&iov[1], v, sizeof(v));
	return devctlv(g->fd, DCMD_VOLUME, 2, 0, iov, NULL, NULL);
}

int iom_attach_input(struct iom *g, uint64_t track, const char *url)
{
	struct iovec iov[2];

	SETIOV(&iov[0], &track, sizeof(track));
	SETIOV(&iov[1], url, strlen(url) + 1);
	return devctlv(g->fd, DCMD_INPUT, 2, 0, iov, NULL, NULL);
}

int iom_detach_input(struct iom *g)
{
	return devctl(g->fd, DCMD_DETACH_INPUT, NULL, 0, NULL);
}

int iom_play(struct iom *g)
{
	return devctl(g->fd, DCMD_PLAY, NULL, 0, NULL);
}

int iom_error_info(struct iom *g, struct iom_err *e)
{
	memset(e, 0, sizeof(*e));
	return devctl(g->fd, DCMD_ERROR_INFO, e, sizeof(*e), NULL);
}

int iom_read_events(struct iom *g, void *buf, int len)
{
	ssize_t n = _readx(g->fd, buf, (size_t)len, IO_XFLAG_NONBLOCK, NULL, 0);

	if (n < 0)
		return errno == EAGAIN ? 0 : -1;
	return (int)n;
}

void iom_close(struct iom *g)
{
	if (g->fd >= 0)
		close(g->fd);
	g->fd = -1;
}

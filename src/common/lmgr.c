#include "lmgr.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>

#define LM_PATH "/dev/layermanager"
#define LM_TIMEOUT_NS 20000000000ULL /* showScreen's devctl budget */

static int lm_fd = -1;

/* devctl() returns EOK or an errno value; the HMI retries on EINTR.
 * TimerTimeout() arms a timeout for the next blocking kernel call, which is
 * devctl's MsgSendv: a hung layer manager gives ETIMEDOUT, not a hang. */
static int lm_devctl(unsigned dcmd, void *data, size_t nbytes)
{
	int tries = 10, rc;

	do {
		uint64_t timeout = LM_TIMEOUT_NS;

		if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
		                 NULL, &timeout, NULL) == -1)
			return errno;
		rc = devctl(lm_fd, (int)dcmd, data, nbytes, NULL);
	} while (rc == EINTR && --tries > 0);
	return rc;
}

int lm_open(void)
{
	/* qnx-audi / libhybrid open flags: 0x2002 */
	lm_fd = open(LM_PATH, O_RDWR | O_CLOEXEC);
	return lm_fd >= 0 ? 0 : errno;
}

void lm_close(void)
{
	if (lm_fd >= 0)
		close(lm_fd);
	lm_fd = -1;
}

int lm_check_version(int *major, int *minor)
{
	int data[3] = { 0, 0, 0 };
	int rc = lm_devctl(LM_DCMD_CHECK_VERSION, data, sizeof(data));

	*major = data[0];
	*minor = data[1];
	return rc;
}

int lm_register_displayable(int cid, int w, int h, int flags, int *result)
{
	int data[5] = { cid, w, h, flags, 0 };
	int rc = lm_devctl(LM_DCMD_REGISTER_DISPLAYABLE, data, sizeof(data));

	*result = data[4];
	return rc;
}

int lm_unregister_displayable(int cid)
{
	return lm_devctl(LM_DCMD_UNREGISTER_DISPLAYABLE, &cid, sizeof(cid));
}

int lm_get_vfb(int cid, struct lm_vfb *vfb)
{
	int data[4] = { cid, -1, -1, -1 };
	int rc = lm_devctl(LM_DCMD_GET_VFB, data, sizeof(data));

	vfb->sid = (unsigned)data[1];
	vfb->uid = (unsigned)data[2];
	vfb->alpha_sid = (unsigned)data[3];
	return rc;
}

int lm_update_vfb(int cid)
{
	return lm_devctl(LM_DCMD_UPDATE_VFB, &cid, sizeof(cid));
}

int lm_show_displayables(int display, const int *cids, int n)
{
	/* showScreen's buffer: display, n, cids[n] (12 bytes for n == 0).
	 * qnx-audi sends (2 + n) * 4 ints, which the server also accepts. */
	int data[2 + LM_MAX_SHOW];

	if (n < 0 || n > LM_MAX_SHOW)
		return EINVAL;
	memset(data, 0, sizeof(data));
	data[0] = display;
	data[1] = n;
	memcpy(&data[2], cids, n * sizeof(int));
	return lm_devctl(LM_DCMD_SHOW_DISPLAYABLES, data,
	                 (n ? 2 + n : 3) * sizeof(int));
}

int lm_text_command(const char *cmd)
{
	int fd = open(LM_PATH, O_WRONLY);
	ssize_t len = (ssize_t)strlen(cmd), n;

	if (fd < 0)
		return errno;
	n = write(fd, cmd, len);
	close(fd);
	if (n < 0)
		return errno;
	return n == len ? 0 : EIO;
}

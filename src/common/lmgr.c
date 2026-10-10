#include "lmgr.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define LM_PATH "/dev/layermanager"

static int lm_fd = -1;

/* devctl() returns EOK or an errno value; the HMI retries on EINTR. */
static int lm_devctl(unsigned dcmd, void *data, size_t nbytes)
{
	int tries = 10, rc;

	do
		rc = devctl(lm_fd, (int)dcmd, data, nbytes, NULL);
	while (rc == EINTR && --tries > 0);
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

int lm_register_displayable(int cid, int w, int h, int flags)
{
	int data[5] = { cid, w, h, flags, 0 };
	int rc = lm_devctl(LM_DCMD_REGISTER_DISPLAYABLE, data, sizeof(data));

	if (rc == 0 && data[4] == 0)
		rc = EINVAL; /* layer manager reported failure */
	return rc;
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

int lm_cmd_501(int cid)
{
	return lm_devctl(LM_DCMD_501, &cid, sizeof(cid));
}

int lm_show_displayables(int display, const int *cids, int n)
{
	/* Same buffer as qnx-audi's talkToLayerManager(): (2 + n) * 4 ints. */
	int data[64];
	int ints = (2 + n) * 4;

	if (n < 0 || ints > (int)(sizeof(data) / sizeof(data[0])))
		return EINVAL;
	memset(data, 0, sizeof(data));
	data[0] = display;
	data[1] = n;
	memcpy(&data[2], cids, n * sizeof(int));
	return lm_devctl(LM_DCMD_SHOW_DISPLAYABLES, data, ints * sizeof(int));
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

/*
 * hello.c - first native binary for the MMI 3G+: write "hello world" to a
 * file on the SD card.
 *
 * run.sh passes the SD card mount point as argv[1]. If it is missing we
 * probe the usual MMI mount points (slot 1/2, partition t12/t11).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const sd_mounts[] = {
	"/mnt/sdcard10t12", "/mnt/sdcard20t12",
	"/mnt/sdcard10t11", "/mnt/sdcard20t11",
};

static const char *find_sd(void)
{
	unsigned i;

	for (i = 0; i < sizeof(sd_mounts) / sizeof(sd_mounts[0]); i++)
		if (access(sd_mounts[i], W_OK) == 0)
			return sd_mounts[i];
	return NULL;
}

int main(int argc, char *argv[])
{
	const char *sd = argc > 1 ? argv[1] : find_sd();
	char path[256];
	FILE *f;

	if (sd == NULL) {
		fprintf(stderr, "hello: no writable SD card found\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/hello.txt", sd);

	f = fopen(path, "w");
	if (f == NULL) {
		fprintf(stderr, "hello: fopen %s: %s\n", path, strerror(errno));
		return 1;
	}
	fputs("hello world\n", f);
	fflush(f);
	fsync(fileno(f));
	if (fclose(f) != 0) {
		fprintf(stderr, "hello: fclose %s: %s\n", path, strerror(errno));
		return 1;
	}

	printf("hello: wrote %s\n", path);
	return 0;
}

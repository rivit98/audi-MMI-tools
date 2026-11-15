/*
 * ipcprobe.c - see ipcprobe.h. Read-only: /proc devctls and
 * ConnectServerInfo, nothing is sent to the processes looked at.
 */
#include "types.h"
#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/states.h>

#include "ipcprobe.h"
#include "log.h"
#include "proc.h"

#define MAX_PROCS     4
#define MAX_CONNS     512       /* per process, per range */
#define MAX_THREADS   256
#define NAME_MAX_LEN  64
#define PIDIN_TIMEOUT 15        /* s, per pidin call */
#define PIDIN_CALLS   3         /* per process: threads, fd, arg */

struct pinfo {
	const char *want;       /* basename looked for */
	pid_t pid;              /* 0: not found */
};

static struct pinfo procs[] = {
	{ "MMI3GApplication", 0 },
	{ "MMI3GNavigation", 0 },
	{ "servicebroker", 0 },
};
#define NPROCS ((int)(sizeof(procs) / sizeof(procs[0])))

unsigned ipc_probe_max_ms(void)
{
	return (unsigned)(NPROCS * PIDIN_CALLS * (PIDIN_TIMEOUT + 3)) * 1000u + 5000u;
}

/* The executable's path of pid, basename into buf. 0 or -1. */
static int proc_name(pid_t pid, char *buf, usize size)
{
	char path[32];
	struct {
		procfs_debuginfo info;
		char             more[256];
	} map;
	const char *base;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	memset(&map, 0, sizeof(map));
	if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL) != EOK) {
		close(fd);
		return -1;
	}
	close(fd);
	map.more[sizeof(map.more) - 1] = '\0';
	base = strrchr(map.info.path, '/');
	base = base != NULL ? base + 1 : map.info.path;
	snprintf(buf, size, "%s", base);
	return 0;
}

/* Name of pid for the listing, cached per call site's buffer. */
static const char *name_of(pid_t pid, char *buf, usize size)
{
	if (pid == 1)
		return "procnto";
	if (proc_name(pid, buf, size) != 0)
		snprintf(buf, size, "?");
	return buf;
}

static void find_procs(void)
{
	char name[NAME_MAX_LEN];
	struct dirent *e;
	DIR *d = opendir("/proc");
	int i;

	if (d == NULL) {
		log_msg("step p: /proc: %s", strerror(errno));
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char *end;
		long pid = strtol(e->d_name, &end, 10);

		if (*end != '\0' || pid <= 1)
			continue;
		if (proc_name((pid_t)pid, name, sizeof(name)) != 0)
			continue;
		for (i = 0; i < NPROCS; i++)
			if (procs[i].pid == 0 && strcmp(name, procs[i].want) == 0)
				procs[i].pid = (pid_t)pid;
	}
	closedir(d);
}

/* Is pid one of procs[]? Its index, or -1. */
static int proc_index(pid_t pid)
{
	int i;

	for (i = 0; i < NPROCS; i++)
		if (procs[i].pid != 0 && procs[i].pid == pid)
			return i;
	return -1;
}

/* Every connection of pid in [first, limit): one line each into f; the
 * ones to the other probed processes also into the log. */
static unsigned list_conns(FILE *f, int self, int first, int limit)
{
	struct _server_info info;
	char name[NAME_MAX_LEN];
	unsigned n = 0;
	int coid = first, c;

	while (n < MAX_CONNS) {
		memset(&info, 0, sizeof(info));
		c = ConnectServerInfo(procs[self].pid, coid, &info);
		if (c < 0 || c < coid || (limit > 0 && c >= limit))
			break;
		n++;
		fprintf(f, "  coid %s%-5d -> nd %u pid %-7d chid %-3d scoid %-6d %s\n",
		        c & _NTO_SIDE_CHANNEL ? "side+" : "",
		        c & ~_NTO_SIDE_CHANNEL, (unsigned)info.nd, (int)info.pid,
		        (int)info.chid, (int)info.scoid,
		        name_of(info.pid, name, sizeof(name)));
		if (proc_index(info.pid) >= 0 && info.pid != procs[self].pid)
			log_msg("step p: %s coid %s%d -> %s pid %d chid %d",
			        procs[self].want, c & _NTO_SIDE_CHANNEL ? "side+" : "",
			        c & ~_NTO_SIDE_CHANNEL, procs[proc_index(info.pid)].want,
			        (int)info.pid, (int)info.chid);
		coid = c + 1;
	}
	return n;
}

static const char *state_name(unsigned s)
{
	static const char *const names[] = {
		"DEAD", "RUNNING", "READY", "STOPPED", "SEND", "RECEIVE", "REPLY",
		"STACK", "WAITTHREAD", "WAITPAGE", "SIGSUSPEND", "SIGWAITINFO",
		"NANOSLEEP", "MUTEX", "CONDVAR", "JOIN", "INTR", "SEM", "WAITCTX",
		"NET_SEND", "NET_REPLY",
	};

	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

/* Every thread of pid: state and what it is blocked on. The receive
 * channels go into the log too. The kernel struct may be larger than our
 * header's: the buffer leaves room. */
static unsigned list_threads(FILE *f, int self)
{
	char path[32], name[NAME_MAX_LEN];
	union {
		procfs_status st;
		u8            room[1024];
	} u;
	unsigned n = 0, chids_logged = 0;
	int fd, tid = 1, chids[16], i;

	snprintf(path, sizeof(path), "/proc/%d/as", (int)procs[self].pid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(f, "  %s: %s\n", path, strerror(errno));
		return 0;
	}
	while (n < MAX_THREADS) {
		memset(&u, 0, sizeof(u));
		u.st.tid = tid;
		if (devctl(fd, DCMD_PROC_TIDSTATUS, &u, sizeof(u), NULL) != EOK)
			break;
		if (u.st.tid < tid)
			break;
		n++;
		fprintf(f, "  tid %-3d prio %-3u %-10s", (int)u.st.tid,
		        (unsigned)u.st.priority, state_name(u.st.state));
		switch (u.st.state) {
		case STATE_RECEIVE:
			fprintf(f, " on own chid %d\n", (int)u.st.blocked.channel.chid);
			for (i = 0; i < (int)chids_logged; i++)
				if (chids[i] == (int)u.st.blocked.channel.chid)
					break;
			if (i == (int)chids_logged &&
			    chids_logged < sizeof(chids) / sizeof(chids[0]))
				chids[chids_logged++] = (int)u.st.blocked.channel.chid;
			break;
		case STATE_SEND:
		case STATE_REPLY:
			fprintf(f, " to pid %d (%s) chid %d coid 0x%x\n",
			        (int)u.st.blocked.connect.pid,
			        name_of(u.st.blocked.connect.pid, name, sizeof(name)),
			        (int)u.st.blocked.connect.chid,
			        (unsigned)u.st.blocked.connect.coid);
			break;
		default:
			fprintf(f, "\n");
		}
		tid = u.st.tid + 1;
	}
	close(fd);
	if (chids_logged > 0) {
		char list[16 * 6 + 1];
		usize len = 0;

		list[0] = '\0';
		for (i = 0; i < (int)chids_logged && len + 7 < sizeof(list); i++)
			len += (usize)snprintf(list + len, sizeof(list) - len, "%s%d",
			                       i ? " " : "", chids[i]);
		log_msg("step p: %s threads receive on chids: %s", procs[self].want, list);
	}
	return n;
}

int ipc_probe(const char *out_dir)
{
	char path[256], pidin_path[256], cmd[96];
	static const char *const pidin_fmt[PIDIN_CALLS] = {
		"pidin -p %d", "pidin -p %d fd", "pidin -p %d arg",
	};
	unsigned t0 = log_ms(), nf, ns, nt;
	FILE *f;
	int i, k;

	find_procs();
	for (i = 0; i < NPROCS; i++)
		log_msg("step p: %s: pid %d", procs[i].want, (int)procs[i].pid);

	snprintf(path, sizeof(path), "%s/hello5_ipc.txt", out_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		log_msg("step p: %s: %s", path, strerror(errno));
		return -1;
	}
	for (i = 0; i < NPROCS; i++) {
		if (procs[i].pid == 0) {
			fprintf(f, "=== %s: not running\n\n", procs[i].want);
			continue;
		}
		fprintf(f, "=== %s pid %d\n--- connections\n", procs[i].want,
		        (int)procs[i].pid);
		nf = list_conns(f, i, 0, _NTO_SIDE_CHANNEL);
		ns = list_conns(f, i, _NTO_SIDE_CHANNEL, 0);
		fprintf(f, "--- threads\n");
		nt = list_threads(f, i);
		fprintf(f, "\n");
		fflush(f);
		log_msg("step p: %s: %u fd + %u side-channel connections, %u threads",
		        procs[i].want, nf, ns, nt);
	}
	fclose(f);
	log_msg("step p: listing in %s (%u ms)", path, log_ms() - t0);

	/* the unit's own view, each call bounded (pidin can wedge) */
	snprintf(pidin_path, sizeof(pidin_path), "%s/hello5_pidin.txt", out_dir);
	unlink(pidin_path);
	for (i = 0; i < NPROCS; i++) {
		if (procs[i].pid == 0)
			continue;
		for (k = 0; k < PIDIN_CALLS; k++) {
			int st;

			snprintf(cmd, sizeof(cmd), pidin_fmt[k], (int)procs[i].pid);
			st = proc_shell(pidin_path, PIDIN_TIMEOUT, cmd);
			log_msg("step p: %s: %s", cmd, proc_status_str(st));
		}
	}
	log_msg("step p: done in %u ms", log_ms() - t0);
	return procs[0].pid != 0 && procs[1].pid != 0 ? 0 : -1;
}

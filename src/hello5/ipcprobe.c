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
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/states.h>

#include "ipcprobe.h"
#include "log.h"
#include "pidscan.h"
#include "proc.h"

#define MAX_PROCS     4
#define MAX_CONNS     512       /* per process, per range */
#define MAX_THREADS   256
#define NAME_MAX_LEN  64
#define PIDIN_TIMEOUT 15        /* s, per pidin call */
#define PIDIN_CALLS   2         /* per process: threads, arg ("fd" always ran
                                   into the timeout for the MMI3G processes,
                                   and list_conns has the same) */
#define TOOL_TIMEOUT  10        /* s, sloginfo / pidin arg for the pids */
#define TOOL_CALLS    3        /* + the /proc diagnosis */

struct pinfo {
	const char *want;       /* basename looked for */
	pid_t pid;              /* 0: not found */
	const char *how;        /* where the pid came from */
};

static struct pinfo procs[] = {
	{ "MMI3GApplication", 0, NULL },
	{ "MMI3GNavigation", 0, NULL },
	{ "servicebroker", 0, NULL },
};
#define NPROCS ((int)(sizeof(procs) / sizeof(procs[0])))

unsigned ipc_find_max_ms(void)
{
	return (unsigned)(TOOL_CALLS * (TOOL_TIMEOUT + 3)) * 1000u;
}

static void (*progress)(const char *what, unsigned max_ms);

void ipc_set_progress(void (*cb)(const char *what, unsigned max_ms))
{
	progress = cb;
}

static void say(const char *what, unsigned max_ms)
{
	if (progress != NULL)
		progress(what, max_ms);
}

unsigned ipc_probe_max_ms(void)
{
	return ipc_find_max_ms() +
	       (unsigned)(NPROCS * PIDIN_CALLS * (PIDIN_TIMEOUT + 3)) * 1000u + 5000u;
}

/* Where reading a name went wrong. */
enum { NAME_OK, NAME_OPEN, NAME_DEVCTL };

/* The executable's path of pid, basename into buf: through /proc/<pid>/as
 * and DCMD_PROC_MAPDEBUG_BASE, as the unit's own pidin does. 0, or the
 * errno, with the failing stage in *stage (may be NULL). */
static int proc_name_st(pid_t pid, char *buf, usize size, int *stage)
{
	char path[32];
	struct {
		procfs_debuginfo info;
		char             more[256];
	} map;
	const char *base;
	int fd, rc;

	snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno;
		if (stage != NULL)
			*stage = NAME_OPEN;
		return rc != 0 ? rc : EIO;
	}
	memset(&map, 0, sizeof(map));
	rc = devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL);
	close(fd);
	if (rc != EOK) {
		if (stage != NULL)
			*stage = NAME_DEVCTL;
		return rc;
	}
	map.more[sizeof(map.more) - 1] = '\0';
	base = strrchr(map.info.path, '/');
	base = base != NULL ? base + 1 : map.info.path;
	snprintf(buf, size, "%s", base);
	if (stage != NULL)
		*stage = NAME_OK;
	return 0;
}

static int proc_name(pid_t pid, char *buf, usize size)
{
	return proc_name_st(pid, buf, size, NULL) == 0 ? 0 : -1;
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

/* Does pid exist? kill(pid, 0): EPERM still means it does. */
static int pid_alive(pid_t pid)
{
	return kill(pid, 0) == 0 || errno == EPERM;
}

/* The /proc walk: every pid's name, counting each way it can fail, so a
 * walk that finds nothing says why. */
static unsigned walk_proc(void)
{
	char name[NAME_MAX_LEN], samples[160];
	unsigned entries = 0, pids = 0, named = 0, open_fail = 0, dev_fail = 0;
	int open_err = 0, dev_err = 0, stage, rc, i;
	usize slen = 0;
	struct dirent *e;
	DIR *d = opendir("/proc");

	if (d == NULL) {
		log_msg("procs: opendir /proc: %s", strerror(errno));
		return 0;
	}
	samples[0] = '\0';
	while ((e = readdir(d)) != NULL) {
		char *end;
		long pid;

		entries++;
		pid = strtol(e->d_name, &end, 10);
		if (*end != '\0' || end == e->d_name || pid <= 1)
			continue;
		pids++;
		rc = proc_name_st((pid_t)pid, name, sizeof(name), &stage);
		if (rc != 0) {
			if (stage == NAME_OPEN) {
				open_fail++;
				if (open_err == 0)
					open_err = rc;
			} else {
				dev_fail++;
				if (dev_err == 0)
					dev_err = rc;
			}
			continue;
		}
		named++;
		if (named <= 4 && slen + 40 < sizeof(samples))
			slen += (usize)snprintf(samples + slen, sizeof(samples) - slen,
			                        "%s%ld=%s", slen ? " " : "", pid, name);
		for (i = 0; i < NPROCS; i++)
			if (procs[i].pid == 0 && strcmp(name, procs[i].want) == 0) {
				procs[i].pid = (pid_t)pid;
				procs[i].how = "/proc";
			}
	}
	closedir(d);
	log_msg("procs: /proc: %u entries, %u pids, %u named, open failed %u (%s), "
	        "devctl failed %u (%s)", entries, pids, named, open_fail,
	        open_fail ? strerror(open_err) : "-", dev_fail,
	        dev_fail ? strerror(dev_err) : "-");
	if (named > 0)
		log_msg("procs: first names: %s", samples);
	return named;
}

/* When /proc named nothing or missed one of procs[]: what the shell sees
 * of it, so the reason is on the card rather than a guess. */
static void proc_diag(const char *out_dir)
{
	char path[256], cmd[512];
	usize len;
	int i, st;

	if (out_dir == NULL)
		return;
	snprintf(path, sizeof(path), "%s/hello5_procdiag.txt", out_dir);
	unlink(path);
	len = (usize)snprintf(cmd, sizeof(cmd),
	                      "id; ls -ld /proc /proc/self /proc/1 /proc/%d; "
	                      "ls -l /proc/%d; ls /proc | head -40",
	                      (int)getpid(), (int)getpid());
	for (i = 0; i < NPROCS && len + 64 < sizeof(cmd); i++)
		if (procs[i].pid != 0)
			len += (usize)snprintf(cmd + len, sizeof(cmd) - len,
			                       "; ls -l /proc/%d", (int)procs[i].pid);
	st = proc_shell(path, TOOL_TIMEOUT, cmd);
	log_msg("procs: /proc diagnosis -> hello5_procdiag.txt: %s",
	        proc_status_str(st));
}

/* The pids /proc did not give, from the text of a unit tool run into
 * out_dir/file (sloginfo, pidin arg). */
static void scan_tool(const char *out_dir, const char *file, const char *cmd)
{
	char path[256];
	int i, st, missing = 0;

	for (i = 0; i < NPROCS; i++)
		if (procs[i].pid == 0)
			missing++;
	if (missing == 0 || out_dir == NULL)
		return;
	snprintf(path, sizeof(path), "%s/%s", out_dir, file);
	unlink(path);
	{
		char what[128];

		snprintf(what, sizeof(what), "finding the MMI processes: %s", cmd);
		say(what, TOOL_TIMEOUT * 1000u);
	}
	st = proc_shell(path, TOOL_TIMEOUT, cmd);
	log_msg("procs: %s -> %s: %s", cmd, file, proc_status_str(st));
	for (i = 0; i < NPROCS; i++) {
		pid_t pid;

		if (procs[i].pid != 0)
			continue;
		pid = (pid_t)pidscan_file(path, procs[i].want);
		if (pid == 0)
			continue;
		if (!pid_alive(pid)) {
			log_msg("procs: %s names %s pid %d, but it is gone", cmd,
			        procs[i].want, (int)pid);
			continue;
		}
		procs[i].pid = pid;
		procs[i].how = cmd;
	}
}

/* Once per run: /proc, then sloginfo, then pidin arg for what is left. */
static void find_procs(const char *out_dir)
{
	static int done;
	char name[NAME_MAX_LEN];
	unsigned named;
	int i, stage, rc;

	if (done)
		return;
	done = 1;
	rc = proc_name_st(getpid(), name, sizeof(name), &stage);
	log_msg("procs: uid %d euid %d; own name through /proc: %s%s",
	        (int)getuid(), (int)geteuid(), rc == 0 ? name : strerror(rc),
	        rc == 0 ? "" : stage == NAME_OPEN ? " (open)" : " (devctl)");
	say("finding the MMI processes: /proc", 2000);
	named = walk_proc();
	scan_tool(out_dir, "hello5_sloginfo.txt", "sloginfo");
	scan_tool(out_dir, "hello5_pidin_arg.txt", "pidin arg");
	for (i = 0; i < NPROCS; i++)
		if (procs[i].pid == 0 || strcmp(procs[i].how, "/proc") != 0)
			named = 0;
	if (rc != 0 || named == 0) {
		say("finding the MMI processes: /proc diagnosis", TOOL_TIMEOUT * 1000u);
		proc_diag(out_dir);
	}
	for (i = 0; i < NPROCS; i++)
		log_msg("procs: %s: pid %d (%s)", procs[i].want, (int)procs[i].pid,
		        procs[i].pid != 0 ? procs[i].how : "not found");
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

/* Add chid to chids[] if it is not there yet. The new count. */
static unsigned add_chid(int *chids, unsigned n, unsigned max, int chid)
{
	unsigned i;

	if (chid <= 0 || n >= max)
		return n;
	for (i = 0; i < n; i++)
		if (chids[i] == chid)
			return n;
	chids[n++] = chid;
	return n;
}

/* The chids of procs[self] that procs[other] holds a connection to: plain
 * connections first (HBDSI requests go over those), then side-channel
 * ones (the child's "process ready" message uses a side channel). */
static unsigned conn_chids(int self, int other, int *chids, unsigned n,
                           unsigned max, int side)
{
	struct _server_info info;
	int coid = side ? _NTO_SIDE_CHANNEL : 0;
	int limit = side ? 0 : _NTO_SIDE_CHANNEL;
	unsigned seen = 0;
	int c;

	while (seen < MAX_CONNS) {
		memset(&info, 0, sizeof(info));
		c = ConnectServerInfo(procs[other].pid, coid, &info);
		if (c < 0 || c < coid || (limit > 0 && c >= limit))
			break;
		seen++;
		if (info.pid == procs[self].pid)
			n = add_chid(chids, n, max, (int)info.chid);
		coid = c + 1;
	}
	return n;
}

/* The chids procs[self]'s own threads are RECEIVE-blocked on. */
static unsigned recv_chids(int self, int *chids, unsigned n, unsigned max)
{
	char path[32];
	union {
		procfs_status st;
		u8            room[1024];
	} u;
	int fd, tid = 1;
	unsigned seen = 0;

	snprintf(path, sizeof(path), "/proc/%d/as", (int)procs[self].pid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		log_msg("step d: %s: %s, so not the channels its threads receive on",
		        path, strerror(errno));
		return n;
	}
	while (seen < MAX_THREADS) {
		memset(&u, 0, sizeof(u));
		u.st.tid = tid;
		if (devctl(fd, DCMD_PROC_TIDSTATUS, &u, sizeof(u), NULL) != EOK)
			break;
		if (u.st.tid < tid)
			break;
		seen++;
		if (u.st.state == STATE_RECEIVE)
			n = add_chid(chids, n, max, (int)u.st.blocked.channel.chid);
		tid = u.st.tid + 1;
	}
	close(fd);
	return n;
}

int ipc_app_channels(const char *out_dir, pid_t *app_pid, int *chids,
                     unsigned max, unsigned *n_nav)
{
	unsigned n = 0;

	*n_nav = 0;

	find_procs(out_dir);
	*app_pid = procs[0].pid;
	if (procs[0].pid == 0) {
		log_msg("step d: MMI3GApplication is not running");
		return -1;
	}
	if (procs[1].pid != 0) {
		n = conn_chids(0, 1, chids, n, max, 0);
		log_msg("step d: MMI3GNavigation holds plain connections to %u of "
		        "MMI3GApplication's channels", n);
		n = conn_chids(0, 1, chids, n, max, 1);
		*n_nav = n;
	} else {
		log_msg("step d: MMI3GNavigation is not running, no connection to "
		        "learn the channel from");
	}
	n = recv_chids(0, chids, n, max);
	if (n > 0) {
		char list[16 * 6 + 1];
		usize len = 0;
		unsigned i;

		list[0] = '\0';
		for (i = 0; i < n && len + 7 < sizeof(list); i++)
			len += (usize)snprintf(list + len, sizeof(list) - len, "%s%d",
			                       i ? " " : "", chids[i]);
		log_msg("step d: MMI3GApplication pid %d, candidate chids: %s",
		        (int)procs[0].pid, list);
	}
	return (int)n;
}

int ipc_connected_to(pid_t from, pid_t to_pid, int to_chid)
{
	struct _server_info info;
	int coid = 0, c, side;
	unsigned seen = 0;

	for (side = 0; side < 2; side++) {
		coid = side ? _NTO_SIDE_CHANNEL : 0;
		while (seen < 2 * MAX_CONNS) {
			int limit = side ? 0 : _NTO_SIDE_CHANNEL;

			memset(&info, 0, sizeof(info));
			c = ConnectServerInfo(from, coid, &info);
			if (c < 0 || c < coid || (limit > 0 && c >= limit))
				break;
			seen++;
			if (info.pid == to_pid && (int)info.chid == to_chid)
				return 1;
			coid = c + 1;
		}
	}
	return 0;
}

int ipc_probe(const char *out_dir)
{
	char path[256], pidin_path[256], cmd[96];
	static const char *const pidin_fmt[PIDIN_CALLS] = {
		"pidin -p %d", "pidin -p %d arg",
	};
	unsigned t0 = log_ms(), nf, ns, nt;
	FILE *f;
	int i, k;

	find_procs(out_dir);

	snprintf(path, sizeof(path), "%s/hello5_ipc.txt", out_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		log_msg("step p: %s: %s", path, strerror(errno));
		return -1;
	}
	say("listing connections and threads", 5000);
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
			{
				char what[160];

				snprintf(what, sizeof(what), "%s (%s), call %d of %d", cmd,
				         procs[i].want, i * PIDIN_CALLS + k + 1,
				         NPROCS * PIDIN_CALLS);
				say(what, PIDIN_TIMEOUT * 1000u);
			}
			st = proc_shell(pidin_path, PIDIN_TIMEOUT, cmd);
			log_msg("step p: %s: %s", cmd, proc_status_str(st));
		}
	}
	log_msg("step p: done in %u ms", log_ms() - t0);
	return procs[0].pid != 0 && procs[1].pid != 0 ? 0 : -1;
}

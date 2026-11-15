/*
 * svcscan.c - see svcscan.h. Read-only: the segment list comes from a
 * /proc devctl, the memory from read()s of /proc/<pid>/as. Nothing is
 * written and nothing is sent to the process.
 */
#include "types.h"
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/procfs.h>

#include "log.h"
#include "svcid.h"
#include "svcscan.h"

#define MAX_SEGS      512       /* segments of the process we look at */
#define CHUNK         (64 * 1024)
#define MAX_BYTES     (192u << 20)      /* read at most this much per pass */
#define MAX_MS        20000     /* and stop after this long in all */
#define MAX_NAME_ADDR 32        /* copies of the name we keep */
#define MAX_WINDOWS   24        /* dumped places, so the report stays small */
#define WINDOW_BACK   0x40      /* bytes dumped before a hit */
#define WINDOW_LEN    0x100     /* and in all */
#define ADDR_LIMIT    0x7ff00000u       /* above this an address needs lseek64 */

/* read len bytes of pid's memory at addr. Bytes read, or -1. */
static isize read_at(int fd, u32 addr, void *buf, usize len)
{
	usize got = 0;
	isize n;

	if (addr >= ADDR_LIMIT)
		return -1;
	if (lseek(fd, (off_t)addr, SEEK_SET) == (off_t)-1)
		return -1;
	while (got < len) {
		n = read(fd, (u8 *)buf + got, len - got);
		if (n <= 0)
			break;
		got += (usize)n;
	}
	return got > 0 ? (isize)got : -1;
}

/* The words around addr, into the report; every word that could be a
 * server id into res. */
static void dump_window(FILE *f, int fd, u32 addr, const char *what,
                        struct svc_result *res)
{
	u8 buf[WINDOW_LEN];
	u32 base = (addr - WINDOW_BACK) & ~3u;
	isize got = read_at(fd, base, buf, sizeof(buf));
	isize off;

	fprintf(f, "--- %s at 0x%x\n", what, (unsigned)addr);
	if (got < 4) {
		fprintf(f, "    unreadable\n");
		return;
	}
	for (off = 0; off + 16 <= got; off += 16) {
		u32 w[4];
		int i;

		memcpy(w, buf + off, sizeof(w));
		fprintf(f, "    0x%08x:", (unsigned)(base + (u32)off));
		for (i = 0; i < 4; i++)
			fprintf(f, " %08x", (unsigned)w[i]);
		fprintf(f, "   ");
		for (i = 0; i < 16; i++) {
			u8 c = buf[off + i];

			fputc(c >= 0x20 && c < 0x7f ? (int)c : '.', f);
		}
		fprintf(f, "\n");
	}
	res->nids = svc_ids_collect(buf, (usize)got, res->ids, SVC_MAX_IDS,
	                            res->nids);
}

/* One hit: its address, kept if there is room, always counted. */
static void add_hit(u32 addr, u32 *hits, unsigned *nhits, unsigned max)
{
	if (*nhits < max)
		hits[*nhits] = addr;
	(*nhits)++;
}

/* Reads [vaddr, vaddr+size) in chunks that overlap by `keep` bytes (so a
 * hit across a chunk boundary is not missed) and calls scan() on each
 * chunk. Bytes read. */
typedef void (*chunk_fn)(const u8 *buf, usize len, u32 at, void *arg);

static u64 scan_segment(int fd, u32 vaddr, u32 size, usize keep,
                        chunk_fn scan, void *arg)
{
	static u8 buf[CHUNK];
	u32 off = 0;
	u64 bytes = 0;

	while (off < size) {
		u32 want = size - off < CHUNK ? size - off : CHUNK;
		isize got = read_at(fd, vaddr + off, buf, want);

		if (got <= (isize)keep)
			break;
		bytes += (u64)got;
		scan(buf, (usize)got, vaddr + off, arg);
		if ((u32)got < want)
			break;          /* short read: the rest is not there */
		off += (u32)got - (u32)keep;
	}
	return bytes;
}

/* pass 1: the name itself (with its NUL), wherever it is */
struct name_pass {
	const char *name;
	usize       len;
	u32        *hits;
	unsigned    nhits;
};

static void scan_name(const u8 *buf, usize len, u32 at, void *arg)
{
	struct name_pass *p = arg;
	const u8 *end = buf + len;
	const u8 *q = buf;

	while (len >= p->len && (q = memchr(q, p->name[0], (usize)(end - q))) != NULL) {
		if ((usize)(end - q) >= p->len &&
		    memcmp(q, p->name, p->len) == 0)
			add_hit(at + (u32)(q - buf), p->hits, &p->nhits, MAX_NAME_ADDR);
		q++;
		if (q >= end)
			break;
	}
}

/* pass 2: a 4-byte word pointing at one of the name's addresses */
struct ptr_pass {
	const u32 *want;
	unsigned   nwant;
	u32        lo, hi;
	u32       *hits;
	unsigned   nhits;
};

static void scan_ptr(const u8 *buf, usize len, u32 at, void *arg)
{
	struct ptr_pass *p = arg;
	usize off;

	for (off = 0; off + sizeof(u32) <= len; off += sizeof(u32)) {
		u32 v;
		unsigned i;

		memcpy(&v, buf + off, sizeof(v));
		if (v < p->lo || v > p->hi)
			continue;
		for (i = 0; i < p->nwant; i++)
			if (p->want[i] == v) {
				add_hit(at + (u32)off, p->hits, &p->nhits, MAX_NAME_ADDR);
				break;
			}
	}
}

unsigned svc_scan_max_ms(void)
{
	return MAX_MS + 5000;
}

int svc_scan(pid_t pid, const char *name, const char *out_path,
             struct svc_result *res)
{
	static procfs_mapinfo maps[MAX_SEGS];
	u32 name_addr[MAX_NAME_ADDR], ptr_addr[MAX_NAME_ADDR];
	struct name_pass np;
	struct ptr_pass pp;
	unsigned windows = 0, kept;
	unsigned t0 = log_ms();
	char path[64];
	int fd, nmaps = 0, i, k;
	FILE *f;

	memset(res, 0, sizeof(*res));
	memset(&np, 0, sizeof(np));
	memset(&pp, 0, sizeof(pp));
	np.name = name;
	np.len = strlen(name) + 1;      /* with the NUL: the whole name */
	np.hits = name_addr;

	snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		res->error = errno;
		log_msg("step d: %s: %s", path, strerror(errno));
		return -1;
	}
	if (devctl(fd, DCMD_PROC_MAPINFO, maps, sizeof(maps), &nmaps) != EOK) {
		res->error = errno;
		log_msg("step d: %s: segment list: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (nmaps > MAX_SEGS)
		nmaps = MAX_SEGS;
	res->segments = (unsigned)nmaps;
	f = fopen(out_path, "w");
	if (f == NULL) {
		log_msg("step d: %s: %s", out_path, strerror(errno));
		close(fd);
		return -1;
	}
	fprintf(f, "=== pid %d, %d mapped segments, looking for \"%s\"\n",
	        (int)pid, nmaps, name);
	for (i = 0; i < nmaps; i++)
		fprintf(f, "  0x%08x %9u bytes flags 0x%08x ino %llu %s\n",
		        (unsigned)maps[i].vaddr, (unsigned)maps[i].size,
		        (unsigned)maps[i].flags, (unsigned long long)maps[i].ino,
		        maps[i].ino != 0 ? "file" : "anonymous");

	/* the name itself, anywhere: the binary's own copies of the string
	 * literal, and any copy a service record holds inline */
	for (i = 0; i < nmaps; i++) {
		if (res->bytes > MAX_BYTES || log_ms() - t0 > MAX_MS)
			break;
		res->bytes += scan_segment(fd, (u32)maps[i].vaddr, (u32)maps[i].size,
		                           np.len - 1, scan_name, &np);
	}
	res->name_hits = np.nhits;
	kept = np.nhits < MAX_NAME_ADDR ? np.nhits : MAX_NAME_ADDR;
	fprintf(f, "\n=== %u place(s) hold the name (%u kept)", np.nhits, kept);
	for (i = 0; i < (int)kept; i++)
		fprintf(f, "%s0x%x", i % 8 == 0 ? "\n  " : " ", (unsigned)name_addr[i]);
	fprintf(f, "\n\n");

	/* and every pointer to one of them in anonymous memory: a service
	 * record either holds the name inline or points at a literal */
	if (kept > 0) {
		pp.want = name_addr;
		pp.nwant = kept;
		pp.hits = ptr_addr;
		pp.lo = pp.hi = name_addr[0];
		for (i = 1; i < (int)kept; i++) {
			if (name_addr[i] < pp.lo)
				pp.lo = name_addr[i];
			if (name_addr[i] > pp.hi)
				pp.hi = name_addr[i];
		}
		for (i = 0; i < nmaps; i++) {
			if (maps[i].ino != 0)
				continue;       /* file-backed: not the registry */
			if (log_ms() - t0 > MAX_MS)
				break;
			res->bytes += scan_segment(fd, (u32)maps[i].vaddr,
			                           (u32)maps[i].size, sizeof(u32) - 1,
			                           scan_ptr, &pp);
		}
	}
	res->ptr_hits = pp.nhits;
	fprintf(f, "=== %u place(s) point at it\n\n", pp.nhits);

	/* the words around every hit, so the record's layout can be read */
	for (i = 0; i < (int)kept && windows < MAX_WINDOWS; i++) {
		int anon = 0;

		for (k = 0; k < nmaps; k++)
			if (maps[k].ino == 0 && name_addr[i] >= (u32)maps[k].vaddr &&
			    name_addr[i] < (u32)maps[k].vaddr + (u32)maps[k].size)
				anon = 1;
		if (!anon)
			continue;       /* a copy in the binary tells us nothing */
		dump_window(f, fd, name_addr[i], "the name in anonymous memory", res);
		windows++;
	}
	kept = pp.nhits < MAX_NAME_ADDR ? pp.nhits : MAX_NAME_ADDR;
	for (i = 0; i < (int)kept && windows < MAX_WINDOWS; i++) {
		dump_window(f, fd, ptr_addr[i], "a pointer to the name", res);
		windows++;
	}
	svc_ids_rank(res->ids, res->nids);
	fprintf(f, "\n=== %u candidate server id(s)", res->nids);
	for (i = 0; i < (int)res->nids; i++)
		fprintf(f, " 0x%08x", (unsigned)res->ids[i]);
	fprintf(f, "\n");
	fclose(f);
	close(fd);
	res->ms = log_ms() - t0;
	log_msg("step d: memory scan: %u segments, %llu KB read, %u name + %u "
	        "pointer place(s), %u candidate id(s), %u ms (report in %s)",
	        res->segments, (unsigned long long)(res->bytes / 1024),
	        res->name_hits, res->ptr_hits, res->nids, res->ms, out_path);
	for (i = 0; i < (int)res->nids; i++)
		log_msg("step d: candidate server id 0x%08x (subsystem 0x%x, "
		        "component %u)", (unsigned)res->ids[i],
		        (unsigned)(res->ids[i] & 0xffffu),
		        (unsigned)(res->ids[i] >> 16));
	return 0;
}

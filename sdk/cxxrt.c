/*
 * cxxrt.c - what GCC 16's libstdc++.a (built for sh4 Linux / glibc) needs
 * from its C library and QNX 6.3's libc.so.2 does not have. With
 * cxxrt-std.cpp (the libstdc++ members rebuilt for QNX) it makes
 * cxxrt-qnx.o, which shle-nto-g++ links ahead of libstdc++.a into every C++
 * program, so the overrides below win over libstdc++'s own.
 *
 * Overrides (libstdc++'s versions can't work on QNX 6.3):
 *   __cxa_get_globals(_fast)  per-thread exception state. libstdc++ keeps it
 *                             in a __thread variable (glibc TLS); QNX 6.3 has
 *                             no TLS, so here it is a pthread key.
 *   __cxa_guard_*             function-local statics. libstdc++'s wait on a
 *                             Linux futex; these use one recursive mutex.
 * For the unwinder (libgcc_eh.a):
 *   _dl_find_object           glibc 2.35's "which module holds this pc, where
 *                             is its .eh_frame_hdr", for this executable only
 *                             (static libstdc++, no C++ in the .so files).
 * Plain glibc names, mapped onto QNX's: gettext, __errno_location, stderr,
 * secure_getenv, strerror_r (GNU), __isoc23_strto*, getentropy,
 * arc4random, __ioctl_time64, __libc_single_threaded.
 * Debian builds glibc (and so libstdc++.a) with a 64-bit time_t, and its
 * clock ids are Linux's: __clock_gettime64 and __nanosleep64 (std::chrono,
 * std::this_thread::sleep_for) convert both. get_nprocs for
 * std::thread::hardware_concurrency.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* openqnx's (6.4.1) <string.h> declares the XSI strerror_r; the unit's
 * 6.3.2 libc has none, and libstdc++ wants the GNU one (below). */
#define strerror_r qnx_xsi_strerror_r
#include <string.h>
#undef strerror_r
#include <time.h>
#include <unistd.h>

/* ---- exception globals ------------------------------------------------- */

/* libsupc++'s struct __cxa_eh_globals (DWARF unwinder, not ARM EH) */
struct cxa_eh_globals {
	void *caught_exceptions;
	unsigned int uncaught_exceptions;
};

static pthread_key_t eh_key;
static pthread_once_t eh_once = PTHREAD_ONCE_INIT;

static void eh_key_make(void)
{
	if (pthread_key_create(&eh_key, free) != 0)
		abort();
}

struct cxa_eh_globals *__cxa_get_globals(void)
{
	struct cxa_eh_globals *g;

	pthread_once(&eh_once, eh_key_make);
	g = pthread_getspecific(eh_key);
	if (!g) {
		g = calloc(1, sizeof(*g));
		if (!g || pthread_setspecific(eh_key, g) != 0)
			abort();
	}
	return g;
}

struct cxa_eh_globals *__cxa_get_globals_fast(void)
{
	return __cxa_get_globals();
}

/* ---- function-local static guards -------------------------------------- */

/* The first byte of the 64-bit guard is "initialised" (Itanium C++ ABI). The
 * mutex is held from a successful acquire to its release/abort; recursive,
 * so one static's initialiser can use another. */
static pthread_mutex_t guard_mutex;
static pthread_once_t guard_once = PTHREAD_ONCE_INIT;

static void guard_mutex_make(void)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&guard_mutex, &a) != 0)
		abort();
	pthread_mutexattr_destroy(&a);
}

int __cxa_guard_acquire(uint64_t *g)
{
	if (__atomic_load_n((char *)g, __ATOMIC_ACQUIRE))
		return 0;
	pthread_once(&guard_once, guard_mutex_make);
	pthread_mutex_lock(&guard_mutex);
	if (*(char *)g) {
		pthread_mutex_unlock(&guard_mutex);
		return 0;
	}
	return 1;
}

void __cxa_guard_release(uint64_t *g)
{
	__atomic_store_n((char *)g, 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&guard_mutex);
}

void __cxa_guard_abort(uint64_t *g)
{
	(void)g;
	pthread_mutex_unlock(&guard_mutex);
}

/* ---- _dl_find_object --------------------------------------------------- */

/* Our programs are ET_EXEC with the ELF and program headers at the start of
 * the first PT_LOAD (__executable_start, 0x08040000), which the loader
 * maps. */
struct elf32_ehdr {
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf32_phdr {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};

#define PT_LOAD          1
#define PT_GNU_EH_FRAME  0x6474e550

/* glibc's struct dl_find_object on sh4 (DLFO_STRUCT_HAS_EH_DBASE and
 * DLFO_STRUCT_HAS_EH_COUNT are 0) */
struct dl_find_object {
	unsigned long long dlfo_flags;
	void *dlfo_map_start;
	void *dlfo_map_end;
	void *dlfo_link_map;
	void *dlfo_eh_frame;
	unsigned long long dlfo_reserved[7];
};

extern const char __executable_start[];

static uintptr_t exe_start, exe_end, exe_eh_frame_hdr;
static pthread_once_t exe_once = PTHREAD_ONCE_INIT;

static void exe_scan(void)
{
	const struct elf32_ehdr *eh = (const void *)__executable_start;
	const struct elf32_phdr *ph;
	int i;

	if (memcmp(eh->e_ident, "\177ELF", 4) != 0)
		return;
	ph = (const void *)(__executable_start + eh->e_phoff);
	exe_start = (uintptr_t)__executable_start;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > exe_end)
			exe_end = ph[i].p_vaddr + ph[i].p_memsz;
		else if (ph[i].p_type == PT_GNU_EH_FRAME)
			exe_eh_frame_hdr = ph[i].p_vaddr;
	}
}

int _dl_find_object(void *pc, struct dl_find_object *r)
{
	pthread_once(&exe_once, exe_scan);
	if ((uintptr_t)pc < exe_start || (uintptr_t)pc >= exe_end || !exe_eh_frame_hdr)
		return -1;
	memset(r, 0, sizeof(*r));
	r->dlfo_map_start = (void *)exe_start;
	r->dlfo_map_end = (void *)exe_end;
	r->dlfo_eh_frame = (void *)exe_eh_frame_hdr;
	return 0;
}

/* ---- glibc names ------------------------------------------------------- */

/* Read by libstdc++'s reference counts (COW strings, exception_ptr): 0 = use
 * atomic operations. */
char __libc_single_threaded = 0;

/* The __throw_* helpers translate their messages. */
char *gettext(const char *msgid)
{
	return (char *)msgid;
}

int *__errno_location(void)
{
	return &errno;
}

/* glibc's stderr is a FILE * variable; QNX's is a macro, &_Stderr. Used
 * by the verbose terminate handler ("terminate called after throwing ..."). */
static FILE *qnx_stderr(void)
{
	return stderr;
}
#undef stderr
FILE *stderr;

__attribute__((constructor(101))) static void stderr_init(void)
{
	stderr = qnx_stderr();
}

char *secure_getenv(const char *name)
{
	return getenv(name);
}

/* the GNU variant: returns the message, buf may be unused */
char *strerror_r(int err, char *buf, size_t len)
{
	(void)buf;
	(void)len;
	return strerror(err);
}

/* glibc 2.38 redirects strto* here in C2x mode (binary 0b prefixes) */
unsigned long __isoc23_strtoul(const char *s, char **end, int base)
{
	return strtoul(s, end, base);
}

long __isoc23_strtol(const char *s, char **end, int base)
{
	return strtol(s, end, base);
}

unsigned long long __isoc23_strtoull(const char *s, char **end, int base)
{
	return strtoull(s, end, base);
}

long long __isoc23_strtoll(const char *s, char **end, int base)
{
	return strtoll(s, end, base);
}

/* std::random_device's sources (random.o comes in with the COW string
 * code). No kernel entropy call on QNX 6.3; random_device("/dev/urandom")
 * or ("/dev/random") still work, they read the device. */
int getentropy(void *buf, size_t len)
{
	(void)buf;
	(void)len;
	errno = ENOSYS;
	return -1;
}

uint32_t arc4random(void)
{
	return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

/* random_device's entropy() asks the device with RNDGETENTCNT; Debian's
 * glibc is built with a 64-bit time_t, hence the name. */
int ioctl(int fd, int request, ...);

int __ioctl_time64(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	return ioctl(fd, (int)request, arg);
}

/* ---- 64-bit time_t ----------------------------------------------------- */

/* glibc's struct __timespec64 on 32-bit little endian */
struct timespec64 {
	int64_t tv_sec;
	int32_t tv_nsec;
	int32_t pad;
};

/* Linux clock ids -> QNX's (QNX 1 is CLOCK_SOFTTIME, Linux 1 MONOTONIC) */
static clockid_t qnx_clock(clockid_t linux_id)
{
	switch (linux_id) {
	case 0: case 5: return CLOCK_REALTIME;          /* REALTIME(_COARSE) */
	case 2: return CLOCK_PROCESS_CPUTIME_ID;
	case 3: return CLOCK_THREAD_CPUTIME_ID;
	default: return CLOCK_MONOTONIC;                /* MONOTONIC(_RAW, _COARSE), BOOTTIME */
	}
}

int __clock_gettime64(clockid_t id, struct timespec64 *ts)
{
	struct timespec t;

	if (clock_gettime(qnx_clock(id), &t) != 0)
		return -1;
	ts->tv_sec = (uint32_t)t.tv_sec;        /* QNX 6.3's time_t is unsigned 32-bit */
	ts->tv_nsec = t.tv_nsec;
	ts->pad = 0;
	return 0;
}

int __nanosleep64(const struct timespec64 *req, struct timespec64 *rem)
{
	struct timespec r, left;
	int ret;

	r.tv_sec = req->tv_sec > 0x7fffffff ? 0x7fffffff : (time_t)req->tv_sec;
	r.tv_nsec = req->tv_nsec;
	ret = nanosleep(&r, rem ? &left : NULL);
	if (ret != 0 && rem) {
		rem->tv_sec = left.tv_sec;
		rem->tv_nsec = left.tv_nsec;
		rem->pad = 0;
	}
	return ret;
}

int get_nprocs(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);

	return n > 0 ? (int)n : 1;
}

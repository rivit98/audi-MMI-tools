// bits/os_defines.h - QNX 6.3 in place of libstdc++'s gnu-linux one, which
// needs glibc's <features.h> and keys everything on __GLIBC_PREREQ.
#ifndef _GLIBCXX_OS_DEFINES
#define _GLIBCXX_OS_DEFINES 1

// <ctype.h> is QNX's (Dinkumware), not glibc's: no __ctype_b tables.
#define __NO_CTYPE 1

// pthreads are in QNX's libc.so.2, always there: no weak-symbol check
// (__gthread_active_p() is 1).
#define _GLIBCXX_GTHREAD_USE_WEAK 0

#define _GLIBCXX_NATIVE_THREAD_ID pthread_self()

// std::mutex / recursive_mutex (and timed_, __gnu_cxx::__mutex) get
// pthread_mutex_init in the constructor and pthread_mutex_destroy in the
// destructor, not PTHREAD_MUTEX_INITIALIZER and nothing. QNX's kernel keeps
// a sync object per address from the first lock until SyncDestroy; a mutex
// never destroyed leaves it behind, and a new statically initialised one
// at the same address (the same stack slot, a reused heap block) then
// fails its first lock with EINVAL (openqnx ker/nano_sync.c sync_lookup)
// -> std::system_error. hello9 died of that (the second
// HwStream in run_live). gthr-posix.h's own switches; the cost is one
// kernel call per construction and destruction, and std::mutex is no
// longer constexpr-constructible.
#define _GTHREAD_USE_MUTEX_INIT_FUNC 1
#define _GTHREAD_USE_RECURSIVE_MUTEX_INIT_FUNC 1

#endif

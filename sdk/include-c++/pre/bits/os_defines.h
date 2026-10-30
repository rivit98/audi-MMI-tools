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

#endif

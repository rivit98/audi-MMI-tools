// bits/c++config.h - libstdc++'s sh4-linux-gnu configuration, minus what
// glibc has and QNX 6.3's libc.so.2 doesn't. The autoconf part of the real
// header comes after its #include <bits/os_defines.h>, so these can only be
// taken back here, after it.
#include_next <bits/c++config.h>

#ifndef _QNX_CXXCONFIG_FIXUP
#define _QNX_CXXCONFIG_FIXUP

// Not in QNX's libc
#undef _GLIBCXX_HAVE_ALIGNED_ALLOC
#undef _GLIBCXX_HAVE_AT_QUICK_EXIT
#undef _GLIBCXX_HAVE_QUICK_EXIT
#undef _GLIBCXX_HAVE_STRERROR_L
#undef _GLIBCXX_HAVE_TIMESPEC_GET
#undef _GLIBCXX_HAVE_UCHAR_H
#undef _GLIBCXX_USE_UCHAR_C8RTOMB_MBRTOC8_CXX20
#undef _GLIBCXX_USE_UCHAR_C8RTOMB_MBRTOC8_FCHAR8_T
#undef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
#undef _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK
#undef _GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK
#undef _GLIBCXX_HAVE_LINUX_FUTEX

// No TLS on QNX 6.3 (ldqnx sets up no thread pointer): thread_local and
// anything built on it are out.
#undef _GLIBCXX_HAVE_TLS

// The C99 wide-char functions are missing or macros in QNX's <wchar.h>
#undef _GLIBCXX11_USE_C99_WCHAR
#undef _GLIBCXX98_USE_C99_WCHAR
#undef _GLIBCXX_HAVE_WCSTOF
#undef _GLIBCXX_HAVE_VFWSCANF
#undef _GLIBCXX_HAVE_VSWSCANF
#undef _GLIBCXX_HAVE_VWSCANF

// No <complex.h> / <fenv.h> / C99 math in the sysroot's C headers (the
// math functions themselves come from post/math.h)
#undef _GLIBCXX11_USE_C99_COMPLEX
#undef _GLIBCXX98_USE_C99_COMPLEX
#undef _GLIBCXX_USE_C99_COMPLEX_TR1
#undef _GLIBCXX_USE_C99_FENV_TR1

// QNX's <wchar.h> and <string.h> already declare the C++ const overloads
// (wcschr, strchr, memchr, ...)
#define __CORRECT_ISO_CPP_WCHAR_H_PROTO 1
#define __CORRECT_ISO_CPP_STRING_H_PROTO 1

#endif

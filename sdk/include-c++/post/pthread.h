/*
 * pthread.h - QNX's, with PTHREAD_MUTEX_INITIALIZER usable in C++: its
 * _NTO_SYNC_NONRECURSIVE (0x80000000) initialises an int, a narrowing
 * error in a C++ braced initializer (std::mutex's constexpr constructor).
 */
#include_next <pthread.h>

#if defined(__cplusplus) && !defined(_QNX_CXX_PTHREAD_FIXUP)
#define _QNX_CXX_PTHREAD_FIXUP
#undef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER { (int)_NTO_SYNC_NONRECURSIVE, _NTO_SYNC_INITIALIZER }
#endif

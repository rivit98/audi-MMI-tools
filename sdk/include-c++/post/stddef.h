/* stddef.h - QNX's, plus C11's max_align_t (as GCC's <stddef.h> has it). */
#include_next <stddef.h>

#if defined(__cplusplus) && !defined(_GCC_MAX_ALIGN_T)
#define _GCC_MAX_ALIGN_T
typedef struct {
	long long __max_align_ll __attribute__((__aligned__(__alignof__(long long))));
	long double __max_align_ld __attribute__((__aligned__(__alignof__(long double))));
} max_align_t;
#endif

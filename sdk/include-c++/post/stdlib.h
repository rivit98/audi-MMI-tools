/*
 * stdlib.h - QNX's, plus the C99 names it declares in namespace std but
 * leaves out of its own `using std::...` list for the global namespace,
 * where libstdc++'s <cstdlib> looks for them.
 */
#include_next <stdlib.h>

#if defined(__cplusplus) && !defined(_QNX_CXX_STDLIB_FIXUP)
#define _QNX_CXX_STDLIB_FIXUP
using std::lldiv_t; using std::strtof; using std::strtold;
#endif

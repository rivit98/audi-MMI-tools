// bits/ctype_base.h - libstdc++'s gnu-linux ctype_base, whose masks are
// glibc's <ctype.h> classification bits. QNX's <ctype.h> has no _IS*
// names, so they are given here with glibc's (little endian) values: the
// same constants libstdc++.a's ctype<char> was built with. This makes
// <locale>-using headers compile (C++20 <chrono> includes them); the
// locale machinery itself is not usable (sdk/include-c++/README.md).
#ifndef _QNX_CXX_CTYPE_BITS
#define _QNX_CXX_CTYPE_BITS
enum {
  _ISupper = 0x0100, _ISlower = 0x0200, _ISalpha = 0x0400, _ISdigit = 0x0800,
  _ISxdigit = 0x1000, _ISspace = 0x2000, _ISprint = 0x4000, _ISgraph = 0x8000,
  _ISblank = 0x0001, _IScntrl = 0x0002, _ISpunct = 0x0004, _ISalnum = 0x0008
};
#endif
#include_next <bits/ctype_base.h>

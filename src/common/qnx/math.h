/*
 * math.h - stand-in for the cross build: openqnx ships no <math.h> (libm is
 * not in the source leak, see src/hello3/main.c). Declare here what a C file
 * needs and link -lm: the device's libm.so.2 has the real functions. C++
 * gets a full C99 <math.h> from the SDK (sdk/include-c++/post/math.h).
 */
#ifndef COMMON_QNX_MATH_H
#define COMMON_QNX_MATH_H

double frexp(double x, int *exp);

#endif

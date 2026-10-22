/*
 * math.h - stand-in for the cross build: openqnx ships no <math.h> (libm is
 * not in the source leak, see src/hello3/main.c). stb_image_write.h includes
 * it only for frexp() in its HDR writer, which img.c does not use (it
 * is static and dropped). The device's libm.so.2 has the real one.
 */
#ifndef COMMON_QNX_MATH_H
#define COMMON_QNX_MATH_H

double frexp(double x, int *exp);

#endif

/*
 * math.h - C99 <math.h> for the functions in the unit's libm.so.2
 * (Dinkumware: every C99 function in double, float and long double), which
 * openqnx has no header for. libstdc++'s <cmath> builds std:: on these.
 * Classification macros are GCC builtins; the FP_* values are glibc's.
 */
#ifndef _QNX_CXX_MATH_H
#define _QNX_CXX_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef float  float_t;
typedef double double_t;

#define HUGE_VAL   __builtin_huge_val()
#define HUGE_VALF  __builtin_huge_valf()
#define HUGE_VALL  __builtin_huge_vall()
#define INFINITY   __builtin_inff()
#define NAN        __builtin_nanf("")

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define FP_ILOGB0    (-2147483647 - 1)
#define FP_ILOGBNAN  (-2147483647 - 1)

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                           FP_SUBNORMAL, FP_ZERO, x)
#define isfinite(x)          __builtin_isfinite(x)
#define isinf(x)             __builtin_isinf_sign(x)
#define isnan(x)             __builtin_isnan(x)
#define isnormal(x)          __builtin_isnormal(x)
#define signbit(x)           __builtin_signbit(x)
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* f(x), f(x, y), f(x, y, z), each as f / ff / fl */
#define _QM1(n) double n(double); float n##f(float); long double n##l(long double);
#define _QM2(n) double n(double, double); float n##f(float, float); \
	long double n##l(long double, long double);
#define _QM3(n) double n(double, double, double); float n##f(float, float, float); \
	long double n##l(long double, long double, long double);

_QM1(acos) _QM1(asin) _QM1(atan) _QM2(atan2)
_QM1(cos) _QM1(sin) _QM1(tan)
_QM1(acosh) _QM1(asinh) _QM1(atanh) _QM1(cosh) _QM1(sinh) _QM1(tanh)
_QM1(exp) _QM1(exp2) _QM1(expm1) _QM1(log) _QM1(log10) _QM1(log1p) _QM1(log2) _QM1(logb)
_QM1(cbrt) _QM1(fabs) _QM2(hypot) _QM2(pow) _QM1(sqrt)
_QM1(erf) _QM1(erfc) _QM1(lgamma) _QM1(tgamma)
_QM1(ceil) _QM1(floor) _QM1(nearbyint) _QM1(rint) _QM1(round) _QM1(trunc)
_QM2(fmod) _QM2(remainder) _QM2(copysign) _QM2(nextafter)
_QM2(fdim) _QM2(fmax) _QM2(fmin) _QM3(fma)

#undef _QM1
#undef _QM2
#undef _QM3

double frexp(double, int *); float frexpf(float, int *); long double frexpl(long double, int *);
double ldexp(double, int); float ldexpf(float, int); long double ldexpl(long double, int);
double scalbn(double, int); float scalbnf(float, int); long double scalbnl(long double, int);
double scalbln(double, long); float scalblnf(float, long); long double scalblnl(long double, long);
int ilogb(double); int ilogbf(float); int ilogbl(long double);
double modf(double, double *); float modff(float, float *); long double modfl(long double, long double *);
long lrint(double); long lrintf(float); long lrintl(long double);
long long llrint(double); long long llrintf(float); long long llrintl(long double);
long lround(double); long lroundf(float); long lroundl(long double);
long long llround(double); long long llroundf(float); long long llroundl(long double);
double remquo(double, double, int *); float remquof(float, float, int *);
long double remquol(long double, long double, int *);
double nan(const char *); float nanf(const char *); long double nanl(const char *);
double nexttoward(double, long double); float nexttowardf(float, long double);
long double nexttowardl(long double, long double);

#ifdef __cplusplus
}
#endif

#endif

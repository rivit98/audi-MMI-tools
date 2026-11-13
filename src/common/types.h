/*
 * Short, Rust-style names for the fixed-width, size and float types. Used
 * everywhere in place of uint32_t, size_t, double, ... (C and C++).
 * usize/isize are pointer-sized: they also stand in for uintptr_t, ssize_t,
 * intptr_t and ptrdiff_t (all the same width on SH-4 and on the host).
 */
#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef size_t    usize;
typedef ptrdiff_t isize;

typedef float  f32;
typedef double f64;

#endif

# QNX/SH: a divide by zero is fatal, by design

Status: **understood, worked around where it bit us.** Not a bug in our code
or in the compiler — it is how QNX runs the SH4 FPU. Anything we build for
this unit has to assume a float divide by zero kills the process.

On a desktop, `x / 0.0f` quietly yields `inf` and execution carries on. On
this unit the same instruction raises `SIGFPE` and the process dies. That is
deliberate, and it is set up twice before our code ever runs.

## Where the trap is turned on

`dev/openqnx/trunk/lib/c/startup/sh/crt1.S` — the first thing `_start` does,
before `_init_libc` and before `main`, is call libgcc's `__set_fpscr`:

```
start_2:  .long  __set_fpscr
start_3:  .long  0xc0400
```

`0xc0400` = `SH_FPSCR_PR | SH_FPSCR_DN | SH_FPSCR_ENABLE_Z` — double
precision, denormals-as-zero, and **divide-by-zero exceptions enabled**.

`dev/openqnx/trunk/services/system/ker/sh/nano_fpu.c` sets the same bits as
the kernel's per-thread default, and says why:

> we normally don't use FPU exceptions, but in the case of SH we have to,
> because we use the FPU for integer division. Without enabling
> divide-by-zero on the FPU, we wouldn't catch integer divide-by-zero.

So `ENABLE_Z` is not something we can treat as an accident or turn off
casually: integer `/ 0` reporting depends on it.

`__set_fpscr` keeps the non-`PR` bits in both of libgcc's `__fpscr_values`
entries, so `ENABLE_Z` survives every single/double precision switch GCC
emits for `-m4`.

## Reading it back from a crash

The Audi dumper's slog line gives the faulting address, and the core file's
`QNX floating point registers` note ends with FPUL and FPSCR. Decode FPSCR:

| bit | field | meaning |
|---|---|---|
| 21 | `FR` | register bank |
| 19 | `PR` | 1 = double precision |
| 18 | `DN` | denormals are zero |
| 17–12 | `Cause` | E, V, **Z**, O, U, I |
| 11–7 | `Enable` | E, V, **Z**, O, U, I |
| 6–2 | `Flag` | V, **Z**, O, U, I |
| 1–0 | `RM` | rounding mode |

`Z` is bit 15 in `Cause`, bit 10 in `Enable`, bit 5 in `Flag`. An FPSCR of
`0x00048420` therefore reads: `DN` set, divide-by-zero **enabled**, and
divide-by-zero **raised** — `PR` clear because GCC had switched to single
precision for the surrounding float code.

## The case that bit us: hello4's fog probe

`hello4` died with `SIGFPE code=3 fltno=9 ip=78232656 mapaddr=00032656` at
the start of probe `fog`, identically under GCC 12 and GCC 16, so it was
never a compiler problem. `mapaddr` is the offset inside the mapped library,
and `libGLES_CM.so.6`'s text segment starts at vaddr 0, so offset `0x32656`
disassembles directly:

```
32628:  mov     #3,r1          ; fog mode 3 = GL_LINEAR
...
32644:  fmov    @r2,fr2        ; fr2 = fog END   (ctx+12)
32646:  fmov    @r1,fr1        ; fr1 = fog START (ctx+8)
3264e:  lds.l   @r1+,fpscr     ; -> single precision
32652:  fsub    fr1,fr2        ; fr2 = END - START
32654:  fmov    @r7,fr1        ; fr1 = fog END
32656:  fdiv    fr2,fr1        ; fr1 = END / (END - START)   <-- SIGFPE
...
32678:  fdiv    fr2,fr1        ; and again for 1 / (END - START)
```

Whenever fog state changes while `GL_FOG_MODE` is `GL_LINEAR`, the driver
recomputes the two linear-fog coefficients `END/(END-START)` and
`1/(END-START)`. **It has no guard for `END == START`.** Two ways to reach
that:

1. Setting `GL_FOG_MODE` to `GL_LINEAR` first, while the context's fog START
   and END are both still `0.0` (the driver's initial state — the GL spec
   says they should be `0.0`/`1.0`). This is what actually crashed.
2. Writing a new range one call at a time, where the new START equals the old
   END. `hello4`'s probe steps through `{1,3}`, `{3,7}`, `{6,9}`, so
   `glFogf(GL_FOG_START, 3)` with END still `3` would have crashed too — a
   naive "set the range before the mode" fix does not survive this.

### The fix

`gfx_fog_linear()` in `src/common/gfx.c`. It parks the mode at `GL_EXP`
(whose recompute, at `0x32612`, only divides by a nonzero constant) while it
writes START and END, then switches to `GL_LINEAR` so the coefficients are
computed exactly once, from a range that is known good. It also nudges
`end` when a caller passes `end == start`, so no path can hand the driver a
zero divisor.

Use it instead of `glFogf(GL_FOG_MODE/START/END, ...)` anywhere.

## What to remember

- Any float divide where the denominator can be zero is a crash here, not an
  `inf`. Guard the denominator rather than relying on IEEE semantics.
- The same goes for integer `/ 0` and `% 0`: on SH those also go through the
  FPU and raise `SIGFPE`.
- A `SIGFPE` with a high `ip` is in a shared library, not our code. Take
  `mapaddr` and disassemble the matching firmware `.so` from
  `dev/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/mnt/ifs-root/usr/lib/`:

  ```sh
  docker run --rm -v "$PWD/<libdir>":/fw:ro mmi-sdk sh -c '
      dd if=/fw/libGLES_CM.so.6 of=/tmp/w.bin bs=1 skip=$((0x32600)) count=256
      sh4-linux-gnu-objdump -D -b binary -m sh4 -EL --adjust-vma=0x32600 /tmp/w.bin'
  ```

  (The firmware libraries are stripped of section headers, so `objdump -D` on
  the file itself prints nothing — carve the window out and disassemble it as
  raw binary.)

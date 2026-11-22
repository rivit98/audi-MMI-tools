# Rust on the MMI with gccrs

Can GCC's Rust front end (gccrs) build apps for the unit? The test app is
[hello1_rust](../src/hello1_rust/README.md), a Rust port of
[hello](../src/hello/README.md). This page covers the toolchain, what gccrs
compiles, and what a Rust standard library on the unit would take.

Claims about the build come from compiling and disassembling in the
`mmi-gccrs` image (GCC 16.2.0). Nothing here has run on the car yet:
behaviour on the unit is **[unverified]**.

## 1. Result

- gccrs builds a working-looking QNX binary from `#![no_core]` Rust.
  `build/hello1_rust` is 11,152 bytes (the C `hello`: 11,268). It is
  ET_EXEC at `0x08040000`, interpreter `/usr/lib/ldqnx.so.2`, NEEDED only
  `libc.so.2`, and `qnx-elf-fix.py` accepts it.
- Its `main` is the same libc call sequence as `hello.c`: `access`, `snprintf`,
  `mkdir`, `fopen`, `fputs`, `fflush`, `fileno`, `fsync`, `fclose`, `printf`.
  It uses LL/SC code generation and `tools/gusa-scan.awk` finds no gUSA sequence.
- gccrs cannot compile `core`. Without `core` there is no `Option`, no slices'
  methods, no `for` loops, no formatting, no `alloc`, no `std`. Real Rust code
  does not build. gccrs is a curiosity today, not a way to write apps.

## 2. Toolchain

Debian packages gccrs only for x86-64 (`gccrs-16-x86-64-linux-gnu`). There is no
SH4 cross build. [src/hello1_rust/Dockerfile](../src/hello1_rust/Dockerfile)
therefore builds one:

- GCC 16.2.0 from the GNU tarball (pinned SHA-256), `--target=sh4-linux-gnu
  --enable-languages=c,rust`, `make all-gcc` only (driver, `cc1`, `crab1`, no
  target libraries). That takes ~24 minutes on 16 threads.
- `crab1` links two small Rust helpers, `libformat_parser` and `ffi-polonius`.
  GCC's build compiles them with the host's `cargo`/`rustc` (Debian's, inside
  the image) from crates vendored in the tarball, offline.
- The image is `FROM mmi-sdk`. It is the same GCC version as
  `sh4-linux-gnu-gcc-16`, so mmi-sdk's libgcc and crt objects fit.
- `shle-nto-gccrs` compiles the crate root to one object with
  `-frust-incomplete-and-experimental-compiler-do-not-use` and the C apps' code
  generation flags (`-m4a -ml -matomic-model=hard-llcs -fno-pie
  -fno-stack-protector`). It then links with `shle-nto-gcc`, so the QNX link line
  and `qnx-elf-fix.py` are the C apps' own.

## 3. What gccrs 16.2 compiles without `core`

A `#![no_core]` crate declares the lang items the compiler needs
(`#[lang = "sized"]`, `"copy"`) and the builtin macros it uses
(`#[rustc_builtin_macro] macro_rules! concat`). It talks to libc through
`extern "C"`. String literals become C strings with
`concat!(s, "\0") as *const str as *const i8`.

Probe snippets, each compiled to an SH4 object:

| Works | Fails |
|---|---|
| structs, `impl`, methods | `for` over a range: needs lang item `into_iter` (from `core`) |
| enums, `match`, `if let`, `while let` | `Option` / `Result`: in `core` (a crate's own enum works) |
| traits, generics, `&dyn Trait` | slice indexing, `.len()`: the impls are in `core` |
| closures (with a declared `fn_once` lang item) | `let ... else` with a tuple-struct pattern: "sorry, unimplemented" |
| `const fn`, `static mut`, labelled `break`, tuples, `#[repr(C)]` | atomic read-modify-write (`atomic_xadd_*`, `atomic_cxchg_*`): not implemented intrinsics |
| `i64`/`u64`/`f64` arithmetic (calls libgcc's `__udivdi3`, `__floatdidf`) | `#![no_main]`: unknown attribute |
| intrinsics: `offset`, `transmute`, `size_of`, `copy`, atomic load/store, `wrapping_*`, `ctlz`, `bswap` | |

`core` 1.49, whose source GCC ships in `libgrust/rustc-lib/core`, fails in name
resolution after 1.6 s with 1,935 errors (1,310 "could not resolve type path",
248 "cannot find path", 218 "unresolved import"). GCC builds and installs no
`libcore` for any target.

## 4. Generated code: differences from rustc

Seen in the disassembly of the probes and of `build/hello1_rust`:

- Integer overflow checks are kept at `-O2` and call `abort()` directly. rustc's
  release builds drop them. There is no panic machinery.
- Array indexing out of bounds is **not** checked. `a[i]` on `[i32; 4]`
  compiles to a plain load. rustc would panic. This is a memory-safety hole.
- Integer division has no zero check. `a / b` is a bare call to
  `__sdivsi3_i4i`. On the unit a divide by zero is then the usual fatal SIGFPE
  ([qnx-sh-fpu-div0.md](qnx-sh-fpu-div0.md)), not a Rust panic.
- A zero-initialised local array becomes a `memset` call (from `libc.so.2`).

## 5. Toward `std`: what the C++ route would look like for Rust

The C++ apps link GCC's prebuilt `libstdc++.a` statically and patch around QNX
6.3.2 with `sdk/cxxrt.c` and the fix-up headers. Rust can follow the same route
only with a compiler that builds `core`. gccrs doesn't, and LLVM (rustc's normal
back end) has no SH target. The candidate is **rustc_codegen_gcc**: the real
rustc front end with GCC (libgccjit) as back end. **[unverified]**: nothing below
has been built.

| C/C++ today | rustc_codegen_gcc equivalent |
|---|---|
| `-m4a -matomic-model=hard-llcs` | The same options passed to libgccjit. `core::sync::atomic` lowers to GCC's `__atomic` builtins, so the code is LL/SC. `compiler_builtins` must be built the same way, and `make gusa-scan` checks it. |
| static `libstdc++.a` | `-Zbuild-std=core,alloc[,std]` for a custom `sh4a-qnx632.json` target, linked statically |
| `sdk/cxxrt.c` shim | a small shim crate or C file for what QNX 6.3.2's libc lacks |
| no TLS (`qnx-elf-fix.py` rejects it) | `"has-thread-local": false` in the target, so std uses `pthread_key_*` |
| link with `shle-nto-gcc` | `-C linker=shle-nto-gcc`, so `qnx-elf-fix.py` still checks every binary |

Steps, smallest first:

1. **`no_std` + `alloc`**: `core` and `alloc`, a `#[global_allocator]` over libc
   `malloc`/`free`, a `#[panic_handler]` that logs and calls `abort`, and
   hand-written `extern "C"` libc bindings. That gives `Vec`, `String`, `Box`,
   iterators and formatting, which covers what the apps need.
2. **`std`**: Rust's std supports QNX 7 (`*-pc-nto-qnx700`/`qnx710`). The
   `libc` crate's QNX bindings would have to be checked against the 6.3.2
   headers (struct layouts, `stat`, pthread types, errno) in a fork, with host
   tests comparing sizes and offsets against the sysroot.

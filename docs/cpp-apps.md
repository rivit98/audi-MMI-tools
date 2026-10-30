# Writing MMI apps in C++

**Yes, C++ is safe to use for apps, within the rules below.** GCC 16 with
C++20 and its standard library runs on the car: hello8's first 17 checks all
passed on 2026-10-28 (language runtime, exceptions, RTTI, containers,
algorithms, strings, `<chrono>`, `<random>`, threads, mutexes, condition
variables). What doesn't work mostly fails at build time. Two things still
need care at run time:

1. **Atomic read-modify-writes shared between threads.** That means
   `std::atomic` arithmetic and exchanges, and `shared_ptr` copies of one
   object made by several threads. They can lose an update (below). Use
   `std::mutex` for shared state.
2. **`thread_local` and the other things listed under
   [Not available](#not-available).** The build stops on them.

How it works inside (header overlay, runtime shim, ELF fix-up):
[`sdk/include-c++/README.md`](../sdk/include-c++/README.md). The checks
and the car results: [`src/hello8/README.md`](../src/hello8/README.md).

## Building

`shle-nto-g++` is the C++ driver of the SDK's `shle-nto-gcc`. It links
GCC 16's `libstdc++.a` statically, so the binary needs nothing from the car
beyond `libc.so.2` and `libm.so.2` (and `-lgf` etc. if you use them). The
default standard is `gnu++20`.

```sh
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src mmi-sdk \
    shle-nto-g++ -O2 -g -Wall -o myapp main.cpp -lgf
```

In this repo, add the app to `APPS` and `CXX_APPS` in the Makefile and
give it a rule like hello8's:

```make
APPS     := ... myapp
CXX_APPS := hello8 myapp
build/myapp: $(wildcard src/myapp/*.cpp src/myapp/*.h) $(BASE) $(SCREEN)
LIBS_myapp := -lgf
```

The `CXX_APPS` rule compiles the `src/common` C files as C and yours as
C++ in one command. Then add a line to `sd/apps.conf` and run
`make sd SD_APPS=myapp`.

- **`src/common` headers have no `extern "C"`.** Wrap the includes, as
  `src/hello8/main.cpp` does:
  ```cpp
  extern "C" {
  #include "log.h"
  #include "quit.h"
  #include "screen.h"
  }
  ```
- **Size:** about 180 KB stripped for an empty `main()`, ~200 KB with
  containers, strings, exceptions and threads. The libstdc++ runtime
  comes along in every binary.
- **After every link** the wrapper runs `sdk/qnx-elf-fix.py`. It rewrites
  symbol bindings the QNX 6.3 loader doesn't know, and it stops the build
  (exit 1) on what it can't fix: a TLS segment or IFUNC symbols. It
  deletes the binary then, so a rejected build never reaches the card.
- **Test on the build host first.** Code that doesn't touch the car's
  APIs can be built with the host's `g++` and run under ASan/UBSan, as
  `tests/test_hello8.cpp` does (`make test`).

## Safe to use

Everything hello8 checks, all passed on the car:

- static constructors/destructors (`__cxa_atexit` runs them at exit),
  function-local statics (thread-safe init)
- exceptions: throw/catch through many frames, rethrow, nested exceptions,
  `exception_ptr`, exceptions in several threads at once; a throwing
  constructor
- RTTI, `dynamic_cast`, `typeid`, virtual inheritance
- `new`/`delete` incl. aligned and placement `new`, `bad_alloc`
- `std::string`, `string_view`, `to_string`, `stoi`/`stod` and their
  exceptions, `<charconv>`
- every container, `<algorithm>`, `<numeric>`, ranges and views, `span`,
  `optional`, `variant`, `any`, `tuple`, `bitset`
- `std::function`, lambdas, `bind`, `invoke`, `unique_ptr`,
  `shared_ptr`/`weak_ptr` (but see [Threads](#threads-and-atomics))
- `<cmath>` (on the unit's C99 `libm.so.2`), `<complex>`, `<random>`
  engines and distributions
- `<chrono>`: `steady_clock`, `system_clock`, `sleep_for`, the calendar.
  The unit's clock starts at 1970 after a cold boot, so `system_clock` is
  wall time only if something set it.
- `std::thread`, `mutex`, `recursive_mutex`, `shared_mutex`,
  `scoped_lock`, `condition_variable` incl. timed waits, `call_once`

Measured on the car (SH7785 at 792 MHz, one core, `-O2`): throw + catch
through 11 frames 70 µs, sort of 10k ints 2 ms, 10k `std::map` inserts
29 ms, 10k `unordered_map` inserts 26 ms, `sleep_for(100ms)` 101 ms,
mutex lock+unlock ~0.2 µs. Exceptions are fine for errors, but too slow
for control flow in a per-frame loop.

## Threads and atomics

GCC's SH4 atomics are **gUSA** sequences (`-matomic-model=soft-gusa`, the
sh4-linux default). `fetch_add` compiles to:

```
mova   1f, r0        ! r0 = end of the sequence
mov    r15, r1       ! save the stack pointer
mov    #-6, r15      ! r15 = -(length): "inside an atomic sequence"
mov.l  @r2, r3       ! load
add    #1, r3
mov.l  r3, @r2       ! store
1: mov r1, r15       ! restore the stack pointer
```

The Linux kernel sees the negative `r15` on an interrupt and restarts the
sequence. QNX's kernel doesn't know the convention (openqnx
`services/system/ker/sh/kernel.s`, `ker/nano_signal.c`). Two things follow:

- **Lost updates.** If the thread is switched out between the load and the
  store, and another thread updates the same variable, one update is lost
  when the first thread resumes. Nothing crashes, but a counter is off,
  or a `shared_ptr` reference count is off, which means a leak or a double
  free.
- **A signal handler kills the process.** To deliver a signal to a
  handler, QNX builds the signal frame below the thread's `r15`. Inside a
  sequence `r15` is `-6`, which fails the kernel's stack bounds check
  (`WITHIN_BOUNDRY`), so the process is killed instead. Signals that are
  blocked and taken with `sigwait()` never build a frame. That is how
  `src/common/quit.c` takes SIGTERM/INT/HUP.

The window is three instructions, so either needs bad luck. But an app
that runs for hours does billions of such operations. hello8's `atomics`
check (4 threads × 200000 `fetch_add`, 0 lost) doesn't settle it: each
thread finished in under 2 ms, inside one timeslice, so the threads never
interleaved. Its new `atomics stress` check (see below) is built to settle
it.

**Rules:**

- **Share state between threads with `std::mutex` /
  `std::condition_variable`.** They are QNX's pthread objects (the kernel
  does the atomicity), and they are safe. A lock+unlock costs ~0.2 µs.
- **Plain atomic loads and stores are safe:** `std::atomic<int>` or
  `<bool>` `load()`/`store()` compile to single `mov.l`/`mov.b`
  instructions (checked in the disassembly). A
  `std::atomic<bool> stop` flag written by one thread and read by others
  is fine. Don't use `fetch_add`, `exchange`,
  `compare_exchange_*`, `++` or `--` on a variable that another thread
  also modifies.
- **`shared_ptr`: don't copy or drop references to the same object from
  several threads at once** without a lock. Every copy and destruction is
  an atomic increment or decrement. Hand objects between threads under a
  mutex, or use `unique_ptr` and move it.
- **Exceptions thrown and caught in the same thread are fine.** Moving an
  `exception_ptr`, or a copy of a `std::runtime_error` (its message is a
  reference-counted string), to another thread is the same `shared_ptr`
  case.
- **Don't install signal handlers** (`signal`, `sigaction`). Block the
  signals and `sigwait()` for them in a thread, as `quit.c` does.
- **A single-threaded app** only has the signal-handler problem. Following
  the previous rule avoids it.

**A possible fix, being tested:** the SH7785 is an SH-4A and has
load-linked/store-conditional (`movli.l`/`movco.l`; QNX's own
`atomic_add()` uses them). Building with
`-m4a -matomic-model=hard-llcs -Wa,--isa=sh4a` turns `fetch_add` into a
`movli.l`/`add`/`movco.l`/`bf` loop that a context switch can't break: an
interrupt clears the reservation, `movco.l` fails and the loop retries.
It builds with the SDK. The ELF header then says `sh4a`, which QNX's
loader doesn't check (it reads only `e_machine`). It would cover your
own code and every header-inlined template (`std::atomic`, `shared_ptr`
counts). It would not cover what is precompiled into `libstdc++.a`
(`exception_ptr`, exception message strings, `std::thread` internals).
hello8's `atomics stress` check runs a plain load/store control, the gUSA
`fetch_add` and an LL/SC one (`src/hello8/llcs.cpp`, built with these
flags) in 4 round-robin threads for 1.5 s each, and counts the lost updates
of each. Until it has passed on the car, follow the rules above.

## Not available

Fails to link, so you find out at build time:

| Feature | Why | Use instead |
|---|---|---|
| iostreams, `<sstream>`, `<fstream>`, `<format>`, `std::locale`, `<filesystem>` | `libstdc++.a`'s locale code is glibc's (`__uselocale`, ...) | `snprintf`, `fopen`/`fprintf`, POSIX `stat`/`opendir`; the repo's `log.h` for logging |
| `std::async`, futures, `std::latch`/`barrier`/`counting_semaphore`, `atomic::wait`, `jthread` stop tokens | waits on a Linux futex (`syscall`) | `std::thread` + `mutex` + `condition_variable` |
| `std::atomic<long long>` / 64-bit atomics | not lock-free on SH4, needs libatomic | a mutex |
| `notify_all_at_thread_exit` | duplicate `condition_variable` symbols | notify before the thread returns |

Fails at build time through `qnx-elf-fix.py` (exit 1):

| Feature | Why |
|---|---|
| `thread_local`, `__thread` | QNX 6.3 has no TLS (`PT_TLS segment` error) |

Builds, but misbehaves at run time:

| Feature | What happens | Use instead |
|---|---|---|
| an exception escaping a `std::call_once` callable, or thrown through any C library callback (`qsort`, `bsearch`, `pthread_once`, ...) | `std::terminate`: QNX's libc has no unwind tables | catch inside the callback |
| default `std::random_device` | no `getentropy` on QNX 6.3 | `std::random_device("/dev/urandom")` or a seeded engine |
| `std::thread::hardware_concurrency()` | returns 1, correctly: one core | — |

## The car's own C++ libraries

Don't link against them. The unit's `libstdc++.so.5` (GCC 3.3) and the
Dinkumware `libcpp.so.4` / `libecpp-ne.so.4` that MMI3GApplication uses
don't match GCC 16's headers or ABI. Your app carries its own runtime
statically, and the stock apps keep theirs.

## Platform rules that still apply

C++ doesn't change the platform's C-level rules
([`platform-notes.md`](platform-notes.md)):

- **Integer division by zero is a fatal `SIGFPE`**
  ([`qnx-sh-fpu-div0.md`](qnx-sh-fpu-div0.md)). No exception is thrown.
- **The screen, layer manager, input (`/dev/ipc/ch8`), audio (io-media)
  and USB APIs are C.** Call them directly, or wrap them in RAII classes
  that release in their destructors (displayable, `gf` surfaces, io-media
  graph fds), so an exception doesn't leave the MMI's displayable
  registered.
- **Stop cleanly** through `quit.c`: timer, SIGTERM/INT/HUP via
  `sigwait`, SD removal, watchdog.

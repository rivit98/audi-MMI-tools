# Writing MMI apps in C++

**Yes, C++ is safe to use for apps, within the rules below.** GCC 16 with
C++20 and its standard library runs on the car: all 18 of hello8's checks
pass there (language runtime, exceptions, RTTI, containers,
algorithms, strings, `<chrono>`, `<random>`, threads, mutexes, condition
variables, atomics). What doesn't work mostly fails at build time. Two things still
need care at run time:

1. **What `libstdc++.a` keeps of GCC's old atomics.** The SDK builds your
   code with SH-4A LL/SC atomics, so `std::atomic` and `shared_ptr` are
   safe between threads. A few precompiled library parts still use gUSA,
   which loses updates under QNX: exception objects shared between
   threads, `std::thread` start-up, signal handlers ([below](#threads-and-atomics)).
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
  `shared_ptr`/`weak_ptr`, also shared between threads ([Threads](#threads-and-atomics))
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

**The SDK builds with SH-4A LL/SC atomics** (`sdk/shle-nto-gcc` passes `-m4a -matomic-model=hard-llcs -Wa,--isa=sh4a`).
`fetch_add` compiles to:

```
0: movli.l @r4, r0   ! load-linked: load and set the reservation
   add     #1, r0
   movco.l r0, @r4   ! store-conditional: T = 1 if the reservation held
   bf      0b        ! an interrupt in between cleared it: retry
```

A context switch can't break it: the interrupt clears the reservation,
`movco.l` fails and the loop runs again. QNX's own `atomic_add()` works
the same way. The ELF header says `sh4a` then, and QNX's loader doesn't
mind (it checks only `e_machine`).

**GCC's sh4 default, gUSA, is broken on QNX:**

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

- **Lost updates.** A thread switched out between the load and the store
  writes back a stale value when it resumes. That wipes out everything the
  other threads added in between. On the car (hello8 `atomics stress`)
  4 round-robin threads lost **9–64 %** of 129 M gUSA `fetch_add`s
  (11.6 M to 83.2 M). The same loop with LL/SC lost **0 of 129 M**, at
  the same speed. A plain load/store control lost 53 %, so the threads did
  interleave.
- **A signal handler kills the process.** To deliver a signal to a
  handler, QNX builds the signal frame below the thread's `r15`. Inside a
  sequence `r15` is `-6`, which fails the kernel's stack bounds check
  (`WITHIN_BOUNDRY`), so the process is killed instead.

**What is still gUSA:** the code precompiled into `libstdc++.a` and
`libgcc_eh.a`, which comes built for plain sh4. The flags can't change
it. `make gusa-scan` lists every gUSA sequence in the built apps. In
hello8 they are only in:
- the reference-counted (COW) `std::string` that `std::logic_error` /
  `std::runtime_error` keep their message in
- `exception_ptr` (`std::current_exception`, `rethrow_exception`)
- `std::thread` start-up (its `shared_ptr` state)
- `set_terminate` / `set_unexpected` / `set_new_handler`
- the unwinder's frame registry (`btree_*`, `version_lock_*`)

Plus hello8's deliberate `src/hello8/gusa.cpp`. The C apps have none.

**Rules:**

- **Atomics in your own code are safe:** `std::atomic` `fetch_add`,
  `exchange`, `compare_exchange_*`, `++`/`--`, `atomic_flag`, and
  `shared_ptr` copies shared between threads (its counts are inlined from
  the headers, so they are LL/SC too). 8- and 16-bit atomics are emulated
  with 32-bit LL/SC, which is just as safe.
- **Don't pass exception objects between threads at the same time as
  they are copied elsewhere.** Copies of a `std::runtime_error` (its
  message string) and of an `exception_ptr` share a gUSA reference count.
  Throwing and catching in one thread is fine. To hand an error to
  another thread, pass its `what()` text as a `std::string` instead.
- **Start threads from one thread,** or under a mutex. `std::thread`'s
  start-up count is gUSA. Many threads each creating threads at once is
  the case to avoid.
- **Don't install signal handlers** (`signal`, `sigaction`) in C++ apps.
  The precompiled gUSA sequences above can still be running when one
  arrives. Block the signals and `sigwait()` for them in a thread, as
  `src/common/quit.c` does. A C app has no gUSA left, but `sigwait()` is
  still the simpler pattern.
- `std::mutex` / `std::condition_variable` are QNX's pthread objects and
  safe as ever: a lock+unlock costs ~0.2 µs.
- **Don't add `-m4` or another `-matomic-model`** to an app's flags.
  `-m4` stops the build (`atomic model hard-llcs is only available on SH4A
  targets`). `-matomic-model=soft-gusa` brings gUSA back for 8/16-bit
  atomics, and with `,strict` for all of them.

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

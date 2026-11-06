# hello8

The C++ test. hello8 is the first app built with `shle-nto-g++`: GCC 16 and
its libstdc++, linked statically, C++20 ([`sdk/include-c++/README.md`](../../sdk/include-c++/README.md)).
It runs 19 checks of the language runtime and the standard library and
shows each as it finishes: PASS in green, or FAIL in red with the
expectation that failed.

| Check | What it covers |
|---|---|
| static init | constructors before `main`, their order, `__attribute__((constructor))`, function-local statics |
| exceptions | throw/catch through 11 frames with destructors, rethrow, `exception_ptr`, nested exceptions, `uncaught_exceptions`, a throwing constructor; time per throw |
| new/delete | arrays, placement and aligned `new`, `nothrow`, `bad_array_new_length`, `bad_alloc` |
| strings | `std::string` (short and heap), `to_string`, `stoi`/`stoll`/`stod` and their errors (QNX's `errno`), `string_view`, `u16string`, `wstring` |
| algorithms | sort, stable_sort, binary search, partition, nth_element, permutations, set operations, heaps, shuffle, numeric, ranges/views, `span` |
| map/set | `map`, `multimap`, `set`, `multiset`, custom comparators, node extract, 10k random keys against a sorted vector |
| unordered | `unordered_map`/`set`/`multimap`, 10k keys, rehash, custom hash |
| containers | `list`, `forward_list`, `deque`, `queue`, `stack`, `priority_queue`, `bitset`, tuple, `optional`, `variant`, `any` |
| functional | lambdas, `std::function` (small and heap captures), `bind`, `invoke`, `mem_fn` |
| smart pointers | `unique_ptr`, `shared_ptr`/`weak_ptr`, `enable_shared_from_this`, aliasing, custom deleters |
| virtual/RTTI | virtual calls and destructors, `dynamic_cast` (down, cross, virtual bases), `typeid`, `bad_cast` |
| math | `<cmath>` on the unit's `libm.so.2`, classification, `numeric_limits`, `<complex>` |
| random | the standard's fixed outputs of `mt19937`, `mt19937_64` and `minstd_rand0`; distributions |
| chrono | `steady_clock` and `system_clock` against `clock_gettime`/`time`, `sleep_for`, durations, the C++20 calendar |
| threads | 4 threads with a mutex, thread arguments, exceptions in 4 threads at once, one static initialised from 4 threads, recursive/shared mutexes, `scoped_lock` |
| sync | `condition_variable` (producer/consumer, timed wait), `call_once` |
| sync reuse | `mutex`, `recursive_mutex`, `timed_mutex`, `condition_variable` built, locked from two threads, destroyed and built again in the same buffer (3 rounds each); a class holding a mutex made in the same stack frame 3 times and on the heap 3 times. On QNX a mutex never destroyed leaves a kernel sync object at its address, and the next one there fails its first lock with `EINVAL` - the SDK destroys them |
| atomics | the operations, then 4 threads × 200000 `fetch_add`: counts lost updates |
| atomics stress | 4 round-robin threads add to one counter for 1.5 s, three ways: plain load + add + store (the control, must lose updates), `fetch_add` built with GCC's old sh4 default gUSA (`gusa.cpp`, `soft-gusa,strict`), and the same `fetch_add` with the SDK's default SH-4A LL/SC (`llcs.cpp`, must lose none). Reports the lost updates of each |

- **Run:** `hello8 <sd_path> [seconds] [check,check,...]`. Results stay up
  for `seconds` (default 20). The last argument runs only the named
  checks, e.g. `"exceptions,map/set"`.
- **Output:** `hello8.log`: the compiler, C++ standard and type sizes, then
  `check <name> ...` before each check and `PASS`/`FAIL` after it (time,
  notes, the failed expectation), and `RESULT` counts. A line after
  `hello8 exit` says the static destructors ran.
- **If it crashes or hangs:** the last `check ...` line names the check.
  Rerun the others with the third argument.
- **Status:** all checks up to `atomics stress` pass on the car with the
  SDK's LL/SC build, and on the build host with glibc's libstdc++
  (`make test`). `sync reuse` (the SDK's `std::mutex` calls
  `pthread_mutex_init`/`pthread_mutex_destroy`) is built, not yet
  confirmed on the car; hello8 is in the default run for it. Run hello8
  again after changing the SDK's C++ runtime or its atomics model.

## On the car

18 of 18 checks pass (`sync reuse` not yet run). The log says `atomics:
hard-llcs (SH-4A movli.l/movco.l)`, exit 0, and `static destructor ran at
exit` comes last, so `__cxa_atexit` works too. All checks run in about
3.8 s. The system log has nothing from or about hello8. Timings are the
same from run to run:

| | on the car |
|---|---|
| sizes | `std::string` 24, `std::mutex` 8 (= `pthread_mutex_t`), `std::condition_variable` 8 (= `pthread_cond_t`), `std::thread` 4, `std::shared_ptr` 8, `wchar_t` 4, `long double` 8, `time_t` 4 |
| exceptions | throw + catch through 11 frames: 68–70 µs |
| algorithms | sort 10k ints: 2.0 ms |
| map/set | 10k random inserts into `std::map`: 29 ms |
| unordered | 10k inserts into `std::unordered_map`: 26 ms |
| chrono | `sleep_for(100ms)` took 101 ms |
| sync | `wait_for(50ms)` took 50 ms |
| threads | `hardware_concurrency` 1; 4 × 20000 mutex lock/unlock: 16 ms |
| atomics | 4 × 200000 `fetch_add`: 7 ms, 0 lost |

`atomics stress`, 4 `SCHED_RR` threads, 1.5 s per loop:

| loop | adds in 1.5 s | lost | |
|---|---|---|---|
| plain load / add / store | 184.5 M | 95–97 M (52–53 %) | the control: the threads did interleave |
| gUSA `fetch_add` (`gusa.cpp`) | 129.1 M | **11.6–83.2 M (9–64 %)** | **not atomic under QNX** |
| LL/SC `fetch_add` (`-matomic-model=hard-llcs`, the SDK default) | 129.1 M | **0** | atomic, and as fast as gUSA |

**gUSA is broken on QNX, LL/SC fixes it.** A thread switched out inside a
gUSA sequence isn't restarted, and when it resumes it stores a stale
value. That one store wipes out every add the other threads made in the
meantime, which is why a few thousand context switches lose millions of
updates. How much gUSA loses depends on where the context switches land,
so the count varies from run to run; the answer, "not atomic under QNX",
doesn't. LL/SC costs nothing extra: 129.1 M adds both ways. What this
means for apps is in
[docs/cpp-apps.md](../../docs/cpp-apps.md#threads-and-atomics).

## What hello8 taught about the toolchain

**`STB_GNU_UNIQUE` doesn't load.** `libstdc++.a` defines some symbols
(here `_ZNSs4_Rep20_S_empty_rep_storageE`, `std::string`'s empty
representation, old ABI) with binding `STB_GNU_UNIQUE`, a GNU extension.
QNX 6.3's 2008-era `ldqnx` doesn't know that binding, treats the symbol as
undefined and refuses the binary before `main`:

```
unknown symbol: _ZNSs4_Rep20_S_empty_rep_storageE
Could not resolve all symbols
```

So `shle-nto-gcc` runs `sdk/qnx-elf-fix.py` on every program it links,
which turns `UNIQUE` into `GLOBAL` (the same thing in an executable) and
sets the ELF OS/ABI back to System V like the stock binaries. It fails the
build on what it can't fix (IFUNC symbols, a TLS segment). Rebuild the
image with `make sdk`.

**QNX's loader ignores the ELF flags.** A binary built with `-m4a
-matomic-model=hard-llcs -Wa,--isa=sh4a` says `sh4a` in its ELF header
instead of the stock binaries' `sh4`. QNX's loader looks only at
`e_machine` (openqnx `proc/loader_elf.c`), and such a binary loads and
runs. So an app built entirely with these flags runs on the unit.

**The `atomics` check can't fail as written.** The SH7785 has one core,
and 4 × 200000 `fetch_add`s take 7 ms in all, under 2 ms per thread. That
is shorter than QNX's round-robin timeslice (4 ticks), so each thread most
likely runs to the end before the next one starts. Preemption by a
higher-priority thread doesn't matter, because QNX resumes the preempted
thread before our other three. So no two of our threads ever interleave
inside a gUSA sequence, and 0 lost proves nothing [inferred].

**Hence `atomics stress`.** It runs each loop for seconds, so the threads
are switched mid-loop thousands of times. Its three loops, as built for
the car:

```
plain   mov.l @r4,r2 ; clrt ; addc r7,r0 ; add #1,r2 ; mov.l r2,@r4
gUSA    mova 1f,r0 ; mov r15,r1 ; mov #-6,r15 ; mov.l @r4,r8 ; add #1,r8 ; mov.l r8,@r4 ; 1: mov r1,r15
LL/SC   0: movli.l @r4,r0 ; add #1,r0 ; movco.l r0,@r4 ; bf 0b
```

Each runs in 4 threads at once for 1.5 s, with `SCHED_RR` set so that
equal-priority threads take turns every timeslice. A thread switched out
between its load and its store writes back a stale value later, so
updates are lost.

| plain lost | gUSA lost | LL/SC lost | means |
|---|---|---|---|
| 0 | any | any | the threads didn't interleave: inconclusive (FAIL) |
| > 0 | > 0 | 0 | gUSA is broken on QNX, LL/SC fixes it (PASS) |
| > 0 | 0 | 0 | gUSA is safe on QNX after all (PASS) |
| > 0 | any | > 0 | LL/SC is broken too (FAIL) |

**How the two models are built.** `shle-nto-gcc` builds everything with
`-m4a -matomic-model=hard-llcs -Wa,--isa=sh4a`. `llcs.cpp` gets the
default flags and `#error`s if the default model isn't hard-llcs.
`gusa.cpp` is built on its own with `-matomic-model=soft-gusa,strict`.
Without `,strict`, GCC on SH-4A uses `movli.l`/`movco.l` for 32-bit
atomics even in a "soft" model, and the Makefile fails the build if
`hello8-gusa.o` has any. The log starts with the atomics model
(`atomics: hard-llcs`). `make gusa-scan` lists the gUSA sequences left: in
hello8 only `gusa.cpp` and what is precompiled into
`libstdc++.a`/`libgcc_eh.a`
([docs/cpp-apps.md](../../docs/cpp-apps.md#threads-and-atomics)).

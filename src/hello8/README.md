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
| sync reuse | `mutex`, `recursive_mutex`, `timed_mutex`, `condition_variable` built, locked from two threads, destroyed and built again in the same buffer (3 rounds each); a class holding a mutex made in the same stack frame 3 times and on the heap 3 times. On QNX a mutex never destroyed leaves a kernel sync object at its address, and the next one there failed its first lock with `EINVAL` (hello9, 2026-11-05) - the SDK now destroys them |
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
- **Status:** all 18 checks pass on the car (2026-10-30, below) and on the
  build host with glibc's libstdc++ (`make test`). Since then the SDK
  builds everything with LL/SC atomics. hello8 ran once more to confirm
  that build on the car (2026-11-03: 18 of 18, below) and has left the
  default run. Run it again after changing the SDK's C++ runtime or its
  atomics model. **2026-11-05:** back in the default run for the new
  `sync reuse` check (the SDK's `std::mutex` now calls
  `pthread_mutex_init`/`pthread_mutex_destroy`), not yet run on the car.

## On the car

**2026-11-03: 18 of 18 passed with the LL/SC SDK build**
(`runs/3.11.2026 - run1/hello8.log`). The log says `atomics: hard-llcs
(SH-4A movli.l/movco.l)`, exit 0, static destructor ran. Checks 1–17 gave
30.10's timings to the microsecond (throw + catch 68 µs, sort 10k 2.0 ms,
4 × 20000 mutex locks 16 ms). `atomics stress`:

| loop | adds in 1.5 s | lost |
|---|---|---|
| plain load / add / store | 184.6 M | 95.2 M (52 %) |
| gUSA `fetch_add` (`gusa.cpp`, `soft-gusa,strict`) | 129.1 M | **83.2 M (64 %)** |
| LL/SC `fetch_add` (the SDK default now) | 129.1 M | **0** |

The SDK's default atomics work on the car, so hello8 left the default run.
gUSA lost 7× more than on 30.10 (11.6 M). How much it loses depends on
where the context switches land, so the count varies from run to run;
the answer, "not atomic under QNX", doesn't.

**2026-10-25: did not start.** The loader refused the binary before
`main`, in 0 s:

```
unknown symbol: _ZNSs4_Rep20_S_empty_rep_storageE
Could not resolve all symbols
```

The symbol (`std::string`'s empty representation, old ABI) is defined in
hello8 itself, but `libstdc++.a` defines it with binding `STB_GNU_UNIQUE`,
a GNU extension. QNX 6.3's 2008-era `ldqnx` doesn't know that binding and
treats the symbol as undefined. It was the only such symbol in `.dynsym`.

Fixed in the SDK: `shle-nto-gcc` now runs `sdk/qnx-elf-fix.py` on every
program it links, which turns `UNIQUE` into `GLOBAL` (the same thing in an
executable) and sets the ELF OS/ABI back to System V like the stock
binaries. It fails the build on what it can't fix (IFUNC symbols, a TLS
segment). Rebuild the image with `make sdk`.

**2026-10-28: 17 of 17 passed** (`runs/28.10.2026 - c++, sound/`). It
loaded (so the `qnx-elf-fix.py` fix works), ran all checks in 3.8 s, kept the
results up for 20 s and exited 0. `static destructor ran at exit` came
last, so `__cxa_atexit` works too. The system log has nothing from or
about hello8.

| | on the car |
|---|---|
| sizes | `std::string` 24, `std::mutex` 8 (= `pthread_mutex_t`), `std::condition_variable` 8 (= `pthread_cond_t`), `std::thread` 4, `std::shared_ptr` 8, `wchar_t` 4, `long double` 8, `time_t` 4 |
| exceptions | throw + catch through 11 frames: 70 µs |
| algorithms | sort 10k ints: 2.0 ms |
| map/set | 10k random inserts into `std::map`: 29 ms |
| unordered | 10k inserts into `std::unordered_map`: 26 ms |
| chrono | `sleep_for(100ms)` took 101 ms |
| sync | `wait_for(50ms)` took 50 ms |
| threads | `hardware_concurrency` 1; 4 × 20000 mutex lock/unlock: 16 ms |
| atomics | 4 × 200000 `fetch_add`: 7 ms, **0 lost** |

**`atomics` can't fail as written.** The SH7785 has one core, and 4 × 200000
`fetch_add`s took 7 ms in all, under 2 ms per thread. That is shorter than
QNX's round-robin timeslice (4 ticks), so each thread most likely ran to the
end before the next one started. Preemption by a higher-priority thread
doesn't matter, because QNX resumes the preempted thread before our other
three. So no two of our threads ever interleaved inside a gUSA sequence,
and 0 lost proves nothing yet [inferred]. To test it, run the loop for
seconds instead of 200000 iterations, so the threads are switched mid-loop
thousands of times.

**Hence `atomics stress`** (added after this run; on the car 2026-10-30, below). Its
three loops, as built for the car:

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

`llcs.cpp` is built on its own with `-m4a -matomic-model=hard-llcs
-Wa,--isa=sh4a` and linked into hello8. That makes hello8's ELF header
say `sh4a` instead of the stock binaries' `sh4`. QNX's loader looks only
at `e_machine` (openqnx `proc/loader_elf.c`), so it should still load,
and if it does, that is also the check that an app built entirely with
these flags can run. It did (2026-10-30).

**2026-10-30: 18 of 18 passed** (`runs/30.10.2026 - media/hello8.log`).
The `sh4a`-flagged binary loaded and ran, so QNX's loader really does
ignore the ELF flags. The first 17 checks gave the same timings as on
28.10 (throw + catch 68 µs, sort 10k 2.0 ms, `sleep_for(100ms)` 101 ms,
4 × 20000 mutex locks 16 ms). Exit 0, static destructor ran.

`atomics stress`, 4 `SCHED_RR` threads, 1.5 s per loop:

| loop | adds in 1.5 s | lost | |
|---|---|---|---|
| plain load / add / store | 184.5 M | 96.9 M (53 %) | the control: the threads did interleave |
| gUSA `fetch_add` (GCC's default) | 129.1 M | **11.6 M (9 %)** | **not atomic under QNX** |
| LL/SC `fetch_add` (`-matomic-model=hard-llcs`) | 129.1 M | **0** | atomic, and as fast as gUSA |

So the second row of the table above holds: **gUSA is broken on QNX,
LL/SC fixes it.** A thread switched out inside a gUSA sequence isn't
restarted, and when it resumes it stores a stale value. That one store
wipes out every add the other threads made in the meantime, which is why
a few thousand context switches lose millions of updates. LL/SC costs
nothing extra here: 129.1 M adds both ways. What this means for apps is in
[docs/cpp-apps.md](../../docs/cpp-apps.md#threads-and-atomics).

**After 2026-10-30: the SDK's default is LL/SC.** `shle-nto-gcc` now
builds everything with `-m4a -matomic-model=hard-llcs -Wa,--isa=sh4a`, so
the roles in hello8 swapped. `llcs.cpp` gets the default flags and
`#error`s if the default model isn't hard-llcs. `gusa.cpp` is built on its
own with `-matomic-model=soft-gusa,strict`. Without `,strict`, GCC on
SH-4A uses `movli.l`/`movco.l` for 32-bit atomics even in a "soft" model,
and the Makefile fails the build if `hello8-gusa.o` has any. The two
earlier runs are unaffected, because their gUSA loop was built with
`-m4`. The log now starts with the atomics model (`atomics: hard-llcs`).
`make gusa-scan` lists the gUSA sequences left: in hello8 only `gusa.cpp`
and what is precompiled into `libstdc++.a`/`libgcc_eh.a`
([docs/cpp-apps.md](../../docs/cpp-apps.md#threads-and-atomics)).

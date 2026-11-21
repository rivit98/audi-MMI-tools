# hello8

C++ runtime test: the first app built with `shle-nto-g++` (GCC 16, static
libstdc++, C++20). Runs 19 checks and shows each as PASS/FAIL. Retired:
everything passes.

- **Run:** `hello8 <sd_path> [seconds] [check,check,...]`
- **Output:** `var/hello8.log` (compiler, type sizes, per-check result and timing)

## Verifies

- Static init/destructors, exceptions, `new`/`delete`, RTTI, virtual calls.
- Standard library: strings, algorithms/ranges, containers, functional,
  smart pointers, `<cmath>`, `<random>`, `<chrono>`.
- Threads, mutexes, condition variables, `call_once`.
- `sync reuse`: sync objects destroyed and rebuilt at the same address.
- Atomics, and `atomics stress`: plain vs gUSA vs LL/SC `fetch_add` under
  round-robin preemption.

## Results

**Works**
- 19/19 checks pass; static destructors run at exit.
- Exceptions ~70 µs through 11 frames; `sleep_for`/`wait_for` accurate.
- LL/SC atomics (the SDK default) lose 0 updates.
- Mutexes reused at the same address work with the SDK's init/destroy fix.

**Doesn't work**
- gUSA atomics (GCC's sh4 default) lose 9–64 % of updates on QNX.
- `STB_GNU_UNIQUE` symbols don't load (`sdk/qnx-elf-fix.py` rewrites them).

Details: [docs/cpp-apps.md](../../docs/cpp-apps.md), [sdk/include-c++](../../sdk/include-c++/README.md).

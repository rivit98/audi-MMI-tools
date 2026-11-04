# sh4 GCC 12.2 miscompile: `a == 0 && b == 0` loses its second test

Status: **fixed by changing compiler, and confirmed on the car.** The
`mmi-sdk` image now uses Debian sid's `gcc-16-sh4-linux-gnu`
(`16.2.0-3cross1`), which compiles every case below correctly at
`-O1`/`-O2`/`-O3`/`-Os`. The build is back at `-O2`. The GCC 12 root cause
(which commit fixed it) was never pinned down. The analysis below is kept as
the record.

Verified with GCC 16.2 (2026-10-10): `sdk/canary.c` keeps both tests;
`quit_poll` and the inlined `quit_request` in hello2 have all their
loads/tests; the scan heuristic below flags only literal pools in all six
apps; dynamic imports match the GCC 12 `-O0` build (minus `memset`/`strlen`/
`memcmp`, now inlined).

Confirmed on the car the same day. `lmprobe` carried an on-car copy of the
canary, compiled twice (once at the build's flags, once forced to `-O2`).
Under the old toolchain it reported:

```
build flags: (0,&0)=22 (0,&1)=0   correct
forced -O2:  (0,&0)=22 (0,&1)=22  MISCOMPILED
```

and under GCC 16 both lines read `correct`. That was the question the on-car
canary existed to answer, so it has been **removed** (`src/lmprobe/canary.*`
and `log_canary()` in `src/lmprobe/main.c`).

**The build-time canary stays.** `sdk/canary.c` plus the `build/.canary` rule
in the Makefile compile the test case with the apps' real `CFLAGS` and fail
the build if the second compare is gone. It costs one compile of a six-line
file, and the SDK image is built `FROM debian:sid-slim`, a rolling target:
the package pin (`gcc-16-sh4-linux-gnu`) fixes only the major version, so a
later `make sdk` can pull a different GCC 16.x. Since the original bug was a
backend bug in `sh_treg_combine` that was never root-caused, there is no
reason to assume a future 16.x cannot reintroduce it. The canary is what
turns that into a build failure instead of another week on the car.

## Summary

The `mmi-sdk` image's compiler (Debian bookworm `gcc-12-sh4-linux-gnu`
`12.2.0-13cross1`, i.e. `sh4-linux-gnu-gcc (Debian 12.2.0-13) 12.2.0`)
deletes the second comparison of a short-circuit `== 0 && == 0` test at
`-O1`, `-O2`, `-O3` and `-Os`, with `-m4` or `-m4a`, with or without the
`shle-nto-gcc` wrapper flags. A `volatile` read doesn't stop it.
`__attribute__((optimize("O2")))` on one function reproduces it in an `-O0`
build. `-O0` compiles it correctly.

The GIMPLE is correct. The comparison and its load are removed in the SH
backend pass `sh_treg_combine` (the instance after `split1`, dumped as
`*.r.sh_treg_combine2`). Its own dump says it took a plain memory load for
an "inverted condition store" and eliminated it.

## Minimal reproduction

```c
/* m.c */
int g(int a, const int *p) { if (a == 0 && *p == 0) return 22; return a; }
```

```sh
docker run --rm -v $PWD:/w -w /w mmi-sdk sh4-linux-gnu-gcc -O2 -S -o - m.c
```

Wrong (`-O2`): `*p` is never loaded, and `bt` reuses the T bit of the first test:

```
g:  tst   r4,r4
    bf/s  .L12
    mov   r4,r0
    bt    .L13        ; T is still (a == 0): always taken here
.L12: rts
    nop
.L13: rts
    mov   #22,r0
```

Right (`-O0`): two `tst`s and a load through `p`.

So `g(0, &one)` returns 22 instead of 0. `src/lmprobe/canary.c` runs exactly
this on the car (forced `-O2` vs the build's `-O0`), and `sdk/canary.c` plus
the `build/.canary` Makefile rule refuse to build if the apps' `CFLAGS`
produce fewer than two compares.

## How it was found

First car run (2026-10-10, `runs/10.10.2026 - first version run/`): hello2–4
logged `lm_register_displayable(cid=40, …): Invalid argument`. The layer
manager had in fact accepted every register: the slog shows
`processPendingShowRequests: showDisplayable(cid=40)` during each app, and the
next showScreen's `unregister(40)` found the displayable still there. Our
check was

```c
if (rc == 0 && data[4] == 0)
	rc = EINVAL;
```

and `build/hello2` compiled it as `tst r0,r0; bf ret; bf ret; mov #22,r0`,
with no load of `data[4]`.

## Where it is lost (lmgr.c at -O2)

Dump everything: `shle-nto-gcc -O2 -Isrc/common -S -fdump-tree-optimized -fdump-rtl-all src/common/lmgr.c`.
SH target passes are registered late, so their dump numbers are out of order.
`-fdump-passes` gives the real order:

```
rtl-split1             : ON
rtl-sh_treg_combine2   : ON     <- runs here, dump file *.337r.sh_treg_combine2
rtl-subreg3            : ON
...
rtl-sh_treg_combine3   : ON
```

- `*.252t.optimized`: correct, `_1 = data[4]; if (_1 == 0)`.
- `*.289r.split1`: insns 58 (`r189 = mem[r191+16]`, data[4]) and 59
  (`T = r189 == 0`) still there.
- `*.290r.subreg3`: both gone.
- `*.337r.sh_treg_combine2`, for the cbranch in bb 7:

```
set of ccreg:
(insn 59 ... (set (reg:SI 147 t) (eq:SI (reg:SI 189 [ data[4] ]) (const_int 0))))
tracing (reg:SI 189 [ data[4] ])
(insn 58 ... (set (reg:SI 189 [ data[4] ])
        (mem/c:SI (plus:SI (reg/f:SI 191) (const_int 16)) [1 data[4]+0 S4 A32])))
inverted condition store                   <- a memory load, classified as r189 = !T
tracing ccreg
set of ccreg not found
cstore count = 0  inverted cstore count = 1
will try to eliminate inverted cstore
try_eliminate_cstores
...
removing cstore in [bb 7]
deleting insn with uid = 58.
removing test insn before cbranch
```

So the leading hypothesis: the cbranch-trace code that recognises a
"cstore" (`reg = T` / `reg = !T`) misclassifies `(set reg (mem …))` as an
inverted cstore. Then `try_eliminate_cstores` "simplifies"
`T = (!T == 0)` to `T`, which leaves the stale T of the previous `rc == 0`
test.

## Analysis to do

1. **Read the pass.** `gcc/config/sh/sh_treg_combine.cc` at the
   `releases/gcc-12.2.0` tag (Debian `12.2.0-13` = 12.2.0 + Debian patches).
   Look at how a trace entry is classified as a cstore / inverted cstore
   (`is_cmp_eq_zero`, `is_inverted_ccreg`, `find_set_of_reg`,
   `bb_entry::cstore_type`, `trace_reg_uses`, `try_eliminate_cstores`). Find
   why a load (`movsi_ie` from memory) gets `cstore_inverted`.
   - Likely suspect: when tracing the ccreg *before* insn 58 finds no set in
     the BB ("set of ccreg not found"), the classification falls back to a
     default that means "inverted".
2. **Check Debian's patches** for this package
   (`apt-get source gcc-12-cross-ports` or sources.debian.org) for anything
   touching `config/sh/`.
3. **Check newer compilers.** Debian trixie cross-toolchain-ports (GCC 13/14
   for sh4, if packaged), a crosstool-ng sh4 toolchain, and GCC trunk.
   Compiler Explorer has no sh4 target as far as I know, so build or install
   locally. Run `m.c` and `sdk/canary.c` on each.
4. **Search GCC Bugzilla** (component `target`, keyword `sh_treg_combine`,
   or `sh` + "cstore") for a known report and a fix commit to backport.
5. **Reduce further / vary.** Does it need `== 0` on both sides? Try `!= 0`,
   `== 5`, `||`, a register-held second operand instead of a load (the
   `quit_poll` case below compares a loaded global too). This shows how
   wide the pattern is.
6. **Decide the toolchain.** Options:
   - stay at `-O0` (current);
   - a fixed newer GCC;
   - a locally patched GCC 12;
   - patch the pass out (there's no option to disable it:
     `-fdisable-rtl-sh_treg_combine2` is rejected as an unknown pass,
     because target passes are registered after option processing).

   Whatever is chosen, keep `build/.canary`, and extend `sdk/canary.c` with
   the variants from step 5 that also fail.
7. **Cost of `-O0`.** If staying at `-O0` matters for hello3/hello4 (CPU-side
   work per frame), measure fps at `-O0` vs. a fixed compiler. Most
   rendering is on the GPU.

## Also hit in the first-run binaries (built at -O2)

- `lm_register_displayable` in hello2, hello3, hello4 (the visible failure).
- `quit_poll`: `if (quit_reason == QUIT_NONE && sd != NULL && …)` became a
  `bf` straight after `jsr log_ms`, with no load or test of `quit_reason` and
  no `sd != NULL` test. The branch used whatever T bit `log_ms()` left.
- Inlined `quit_request`: `if (quit_reason != QUIT_NONE || reason == QUIT_NONE)`
  has no compare before its `bf`.
- Unverified: the duplicate-branch scan below also flagged `key_thread`,
  `signal_thread`, hello4 `main`/`mesh_*`/`p_light` and hello5 `main`. Some
  may be real, some false positives. hello5's crash wasn't analysed with
  this in mind.

### The scan heuristic

Flags a conditional branch that follows another conditional branch by
fallthrough, with no T-writing instruction in between:

```awk
# sh4-linux-gnu-objdump -d --no-show-raw-insn BIN | awk -v FILE=BIN -f tscan.awk
/^[0-9a-f]+ <.*>:$/ { fn=$2; pend=0; next }
/^ *[0-9a-f]+:\t/ {
  split($0, a, "\t"); op=a[2]; ins=a[2]" "a[3]
  if (op ~ /^(bt|bf|bt\/s|bf\/s|bt\.s|bf\.s)$/) {
    if (pend) print FILE": "fn" "prevaddr" "prevop" ... "$1" "ins
    pend=1; prevop=op; prevaddr=$1; next
  }
  if (op ~ /^(tst|cmp\/|dt|sett|clrt|shll|shlr|shal|shar|rotl|rotr|rotcl|rotcr|addc|subc|addv|subv|negc|div0|div1|ldc|lds|tas|fcmp|movt|bra|braf|jmp|rts|bsr|jsr)/) { pend=0 }
  next
}
```

Known false positives:
- literal pools after `rts`, where `0x0804xxxx` address words decode as
  `bt.s …; mov.b r0,@(r0,r8)`;
- `bt.s` chains whose delay slot sets T (that insn is printed *after* the
  branch).

A better version would scan the compiler's `.s` output (pools are `.long`
there), reset at labels, and look at delay slots. With the `-O0` build, every
remaining hit is a literal pool.

# sh4 GCC 12.2 miscompile: `a == 0 && b == 0` loses its second test

Status: **avoided by changing compiler, confirmed on the car.** Debian
bookworm's GCC 12.2 drops the second test of a short-circuit `== 0 && == 0`
at `-O1` and above. Debian sid's `gcc-16-sh4-linux-gnu` (`16.2.0-3cross1`)
compiles every case below correctly at `-O1`/`-O2`/`-O3`/`-Os`, so the
`mmi-sdk` image uses it and the build runs at `-O2`. Which GCC commit fixed
the bug was never pinned down.

Checked with GCC 16.2: `sdk/canary.c` keeps both tests; `quit_poll` and the
inlined `quit_request` in hello2 have all their loads/tests; the scan
heuristic below flags only literal pools in all six apps; dynamic imports
match the GCC 12 `-O0` build (minus `memset`/`strlen`/`memcmp`, which GCC 16
inlines).

On the car, a canary compiled twice (once at the build's flags, once forced
to `-O2`) reported under GCC 12:

```
build flags: (0,&0)=22 (0,&1)=0   correct
forced -O2:  (0,&0)=22 (0,&1)=22  MISCOMPILED
```

and under GCC 16 both lines read `correct`. The on-car canary has been
removed; the build-time one stays.

**The build-time canary.** `sdk/canary.c` plus the `build/.canary` rule in
the Makefile compile the test case with the apps' real `CFLAGS` and fail the
build if the second compare is gone. It costs one compile of a six-line
file. The SDK image is built `FROM debian:sid-slim`, a rolling target: the
package pin (`gcc-16-sh4-linux-gnu`) fixes only the major version, so a
later `make sdk` can pull a different GCC 16.x. The original bug is a
backend bug in `sh_treg_combine` that was never root-caused, so a future
16.x could bring it back. The canary turns that into a build failure
instead of a miscompiled binary on the car.

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

So `g(0, &one)` returns 22 instead of 0. `sdk/canary.c` plus the
`build/.canary` Makefile rule refuse to build if the apps' `CFLAGS` produce
fewer than two compares.

## How it shows up

The layer manager client checked

```c
if (rc == 0 && data[4] == 0)
	rc = EINVAL;
```

and GCC 12 at `-O2` compiled it as `tst r0,r0; bf ret; bf ret; mov #22,r0`,
with no load of `data[4]`. So `lm_register_displayable(cid=40, …)` returned
`Invalid argument` although the layer manager had accepted the register:
the slog shows `processPendingShowRequests: showDisplayable(cid=40)`, and the
next `unregister(40)` finds the displayable still there.

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

## Open questions

- **Which GCC change fixed it.** Read `gcc/config/sh/sh_treg_combine.cc` at
  `releases/gcc-12.2.0` (Debian `12.2.0-13` = 12.2.0 + Debian patches): how a
  trace entry is classified as a cstore / inverted cstore
  (`is_cmp_eq_zero`, `is_inverted_ccreg`, `find_set_of_reg`,
  `bb_entry::cstore_type`, `trace_reg_uses`, `try_eliminate_cstores`), and
  why a load (`movsi_ie` from memory) gets `cstore_inverted`. Likely
  suspect: when tracing the ccreg before insn 58 finds no set in the BB
  ("set of ccreg not found"), the classification falls back to a default
  that means "inverted". GCC Bugzilla (component `target`,
  `sh_treg_combine`, or `sh` + "cstore") may have the report.
- **How wide the pattern is.** Does it need `== 0` on both sides? `!= 0`,
  `== 5`, `||`, a register-held second operand instead of a load. Variants
  that also fail on GCC 12 belong in `sdk/canary.c`.

The pass can't be switched off from the command line:
`-fdisable-rtl-sh_treg_combine2` is rejected as an unknown pass, because
target passes are registered after option processing.

## Other code GCC 12 miscompiled (at -O2)

- `lm_register_displayable` in hello2, hello3, hello4 (the visible failure).
- `quit_poll`: `if (quit_reason == QUIT_NONE && sd != NULL && …)` became a
  `bf` straight after `jsr log_ms`, with no load or test of `quit_reason` and
  no `sd != NULL` test. The branch used whatever T bit `log_ms()` left.
- Inlined `quit_request`: `if (quit_reason != QUIT_NONE || reason == QUIT_NONE)`
  has no compare before its `bf`.
- Unverified: the duplicate-branch scan below also flagged `key_thread`,
  `signal_thread`, hello4 `main`/`mesh_*`/`p_light` and hello5 `main`. Some
  may be real, some false positives.

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
there), reset at labels, and look at delay slots. With GCC 16, every
remaining hit is a literal pool.

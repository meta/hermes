# JIT: CI-shaped build/test matrix, release validation, perf sanity

This document is the "how to build and test the JIT" companion to
`doc/JIT.md` (architecture) and `utils/jit/README.md` (the dump-diff
tools). It exists because milestone 6 added a fifth build configuration
(release) and the porting work is otherwise done: what is left to record
is not more architecture, but the exact commands a CI job (or a human
reproducing one) would run, and what each run actually proves. Written
2026-08-25 against `x86-jit` @ `5ec9dcd89`.

## The five configurations

| # | Config | Build dir | Notes |
|---|---|---|---|
| 1 | arm64, qemu-user | `cmake-build-arm64` | Cross-compiled, run under `qemu-aarch64-static`; the only way to execute the JIT without arm64 hardware. See `aarch64/README.md`. |
| 2 | x86-64, HV64, ASan+Debug | `cmake-build-x86jit` | The default/reference x86-64 dev build. |
| 3 | x86-64, HV32 (`HEAP_HV_PREFER32`), ASan+Debug | `cmake-build-x86jit-hv32` | Compressed pointers + boxed doubles + contiguous heap. |
| 4 | x86-64, BOXED (`HEAP_HV_BOXED`), ASan+Debug | `cmake-build-x86jit-boxed` | Boxed doubles only, no pointer compression. |
| 5 | x86-64, Release | `cmake-build-x86jit-rel` | **New in milestone 6.** No ASan, no assertions (`NDEBUG`) — see "Release validation" below for why that matters. |

Configs 2-4 are described together in `doc/JIT.md` ("The heap-value-mode
build matrix"); config 1 is `aarch64/README.md` in full. This document
adds config 5 and then gives the complete command matrix for all five in
one place.

### Configure commands

```sh
# 1. arm64, qemu-user (see aarch64/README.md for host-compiler prerequisites)
cmake -B cmake-build-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/aarch64/aarch64-linux-gnu.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
  -DHERMESVM_ALLOW_JIT=2 \
  -DHERMES_UNICODE_LITE=ON \
  -DIMPORT_HOST_COMPILERS="$PWD/cmake-build-host/ImportHostCompilers.cmake" \
  -DQEMU_RUN_PREFIX="qemu-aarch64-static -L /usr/aarch64-linux-gnu"

# 2. x86-64, HV64, ASan+Debug
cmake -B cmake-build-x86jit -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" -DHERMESVM_ALLOW_JIT=2

# 3. x86-64, HV32, ASan+Debug
cmake -B cmake-build-x86jit-hv32 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
  -DHERMESVM_ALLOW_JIT=2 -DHERMESVM_HEAP_HV_MODE=HEAP_HV_PREFER32

# 4. x86-64, BOXED, ASan+Debug
cmake -B cmake-build-x86jit-boxed -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
  -DHERMESVM_ALLOW_JIT=2 -DHERMESVM_HEAP_HV_MODE=HEAP_HV_BOXED

# 5. x86-64, Release (NEW)
cmake -B cmake-build-x86jit-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMESVM_ALLOW_JIT=2
```

Build each with `cmake --build <dir> --target hermes -j "$(nproc)"` (or
`check-hermes`, which builds `hermes` as a dependency — see below).
Configs 2-5 always use clang per this repo's convention (`CLAUDE.md`,
"Always build with Clang"); config 1's compiler comes from the aarch64
toolchain file. No config other than 1 needs `HERMES_UNICODE_LITE` or
`IMPORT_HOST_COMPILERS` — those are cross-compilation-only concerns.

## Gates: what each test command proves

Six distinct checks recur across the matrix. Naming them once here so
the per-config table below can just list which ones apply.

- **G1 — `test/jit` + `test/jit/x86-64`** (`LIT_FILTER="jit/"` under
  `check-hermes`): the behavioral/compile-status suite. On x86-64,
  70 tests: 68 pass, 1 `XFAIL` (`try-catch-dest-reg.js`, the confirmed
  destination-FR-exclusion bug — see `doc/JIT.md`), 1 unsupported
  (`large_literal_obj.js`, `!slow_debug`). On arm64 the same directory
  yields 46 pass / 1 `XFAIL` / 23 unsupported (the 22 `x86-64/`-only
  files plus the same `!slow_debug` skip).
- **G2 — full `check-hermes`**: the whole lit suite plus unit tests plus
  NAPI, with the JIT compiled in but not forced on (`test/jit` is still
  the only directory that forces it). Confirms the JIT-enabled build
  doesn't regress anything outside its own tests.
- **G3 — dump-baseline / `jit-diff.sh`**: `utils/jit/jit-dump.sh` /
  `jit-diff.sh` (see `utils/jit/README.md` and "Dump-baseline workflow"
  below) — proves an emitter change did or didn't alter the code it
  generates, independent of the differential/behavioral gates.
- **G4 — the stress differential**: `aarch64/jit-stress.js` (the same
  file `test/jit/x86-64/stress.js` wraps as a standing lit gate) run
  interpreter-vs-`-Xjit=force` by hand, in the variants below. Byte-equal
  output is the whole check.
- **G5 — the 497-file sweep**: an ad hoc (not a committed script)
  differential over every `test/hermes/*.js`, plain `hermes` vs.
  `-Xjit=force`, same binary — broader-corpus correctness evidence than
  G1/G4 alone. Run and reported per-file in the milestone-5 gate
  (`doc/JIT.md`, "x86-64 porting notes"); not re-run per milestone-6 task
  since no emitter code changed.
- **G6 — perf sanity** (Release only, new this milestone): timed
  interpreter-vs-`-Xjit=force` on representative hot loops. Not a
  regression gate with a pass/fail threshold — see "Perf sanity" below
  for what bar it does check.

### Commands

```sh
# G1, any config's build dir:
LIT_FILTER="jit/" cmake --build <dir> --target check-hermes -j "$(nproc)"

# G2, any config's build dir (drop LIT_FILTER):
cmake --build <dir> --target check-hermes -j "$(nproc)"

# G3, any config's build dir (see "Dump-baseline workflow" below):
utils/jit/jit-dump.sh -o <dir>/jit-baseline.dump <dir>/bin/hermes
utils/jit/jit-diff.sh /tmp/hermes-before <dir>/bin/hermes

# G4, any config's build dir: five invocations, three diffs (interpreter
# output must be byte-identical to each JIT variant's):
<dir>/bin/hermes aarch64/jit-stress.js > int.out
<dir>/bin/hermes -Xjit=force -Xjit-crash-on-error \
  aarch64/jit-stress.js > jit.out
<dir>/bin/hermes -Xjit=force -Xjit-crash-on-error \
  -Xjit-emit-type-asserts aarch64/jit-stress.js > jit-ta.out
<dir>/bin/hermes -O0 aarch64/jit-stress.js > int0.out
<dir>/bin/hermes -O0 -Xjit=force -Xjit-crash-on-error \
  aarch64/jit-stress.js > jit0.out
diff int.out jit.out && diff int.out jit-ta.out && diff int0.out jit0.out
```

arm64 additionally has `./aarch64/qemu-sanity.sh`, a nine-check smoke
test folding most of G1/G4 into one script (see `aarch64/README.md`).

### Per-config gate matrix

| Config | G1 | G2 | G3 | G4 | G5 | G6 |
|---|---|---|---|---|---|---|
| 1. arm64-qemu | yes (46/1/23) | yes (see README) | yes (own baseline) | yes (`qemu-sanity.sh`) | no | n/a (qemu timings are meaningless — see README "Limitations") |
| 2. x86-64 HV64 ASan | yes (68/1/1) | yes (4332 tests: 4174 pass / 7 xfail / 151 unsupported) | yes (own baseline) | yes | yes (480/497) | n/a (ASan skews timings) |
| 3. x86-64 HV32 ASan | yes (68/1/1) | not run separately (G1 + G4 + G5 are the gate for this config; G2 is HV64's job) | n/a (only HV64 has a stored baseline — the emitted code differs by design across modes, see `doc/JIT.md`) | yes | yes (479/497 — one file crosses the sweep's 10s timeout under this mode's extra decode) | n/a |
| 4. x86-64 BOXED ASan | yes (68/1/1) | not run separately | n/a | yes | yes (480/497) | n/a |
| 5. x86-64 Release | yes (68/1/1) | not run (not requested; G1 is the release-specific gate — see below) | not run this task (no release baseline captured; G3's byte-identical-refactor workflow is a dev-loop tool for the ASan tree, not a release CI gate) | yes | not run (no emitter change to re-verify; G4 already covers behavior) | **yes — new this milestone, see below** |

Why G2 is HV64-only: it is a whole-repository regression check
unrelated to heap-value-mode-specific code paths; running it three times
would be redundant cost for no new coverage (HV32/BOXED get their own
JIT-specific coverage via G1/G4/G5, which do vary by mode). A real CI
pipeline may choose to run G2 on all four x86-64 configs anyway for
defense in depth — that's a cost/coverage tradeoff, not a correctness
requirement documented here.

## Dump-baseline workflow (both architectures)

Full mechanics in `utils/jit/README.md`; the summary needed here is the
loop a change actually goes through:

```sh
cmake --build <dir> --target hermes
cp <dir>/bin/hermes /tmp/hermes-before
# ... make an emitter change ...
cmake --build <dir> --target hermes
utils/jit/jit-diff.sh /tmp/hermes-before <dir>/bin/hermes
```

Both arm64 (`cmake-build-arm64/jit-baseline.dump`) and x86-64
(`cmake-build-x86jit/jit-baseline.dump`) have a captured baseline; both
are untracked build artifacts, recaptured with `jit-dump.sh -o ...` after
an intentional change. Two runs of an *unchanged* binary produce
byte-identical canonicalized dumps on both architectures (verified as
part of Task 2 of this milestone — see `doc/JIT.md`'s x86-64 paragraph
and `utils/jit/README.md`'s "Why not just diff" section for why that
isn't true of the raw `-Xdump-jitcode` output). No release-tree baseline
is captured or expected: the workflow is a dev-loop tool for verifying a
refactor didn't change codegen, which is only useful against the tree
you're actively editing (the ASan+Debug ones).

## Release-build validation (first NDEBUG x86-64 run)

Every prior x86-64 build in this port has been ASan+Debug. `NDEBUG`
strips every `assert()` in the codebase, including ones with load-bearing
side effects if any existed (the classic C++ footgun) — e.g. the
rspDelta bookkeeping counter and the per-instruction invariant checks
that `-Xjit-emit-asserts`/`assertPostInstructionInvariants` rely on. A
Release build is therefore not "the same thing, faster" — it is a
genuinely different configuration that had never been exercised before
this task.

Built `cmake-build-x86jit-rel` per the config-5 command above (Release,
clang, `HERMESVM_ALLOW_JIT=2`, no ASan). `hermes` links cleanly. Ran G1
and G4:

```
LIT_FILTER="jit/" check-hermes, cmake-build-x86jit-rel:
-- Testing: 70 of 4291 tests, 16 threads --
  Expected Passes    : 68
  Expected Failures  : 1
  Unsupported Tests  : 1
```

Matches the ASan-tree numbers exactly (68/1/1) — no NDEBUG-only failure.

G4, all three differential variants against `aarch64/jit-stress.js`,
byte-identical in every case (interpreter vs. `-Xjit=force
-Xjit-crash-on-error`; the same with `-Xjit-emit-type-asserts`; the same
at `-O0`). A `-Xdump-jitcode=2` run over the same file confirms all 31
functions compiled with zero declines — the differential isn't quietly
degrading into "compare the interpreter against itself" the way it would
if something silently fell back.

**Triage verdict: clean.** No failure of any kind turned up under
NDEBUG. This does not prove no assert ever had a load-bearing side
effect anywhere in the backend (that would need exhaustively exercising
every assert's guarded code, which G1/G4 do not claim to do) — it proves
the two standing correctness gates this port has relied on all along are
unaffected by stripping assertions, which is the check this task's brief
asked for.

## Perf sanity (Release build only)

**What this checks.** Per the spec's v1 bar: "perf sane" — the JIT must
not lose to the interpreter on hot loops. This is a floor, not a target;
tuning is explicitly out of scope for this milestone (post-v1 work). No
attempt was made to make either side faster — the numbers below are
as-is.

**Methodology.** `cmake-build-x86jit-rel/bin/hermes`, `/usr/bin/time -f
"%e s"`, 3 runs per side, median reported. `-Xjit=force` compiles every
function before its first call, so its wall time includes JIT compile
time — for loops this hot (seconds of execution against sub-millisecond
compile time each), that cost is amortized to noise, but it is not
subtracted out, so the numbers below are, if anything, a conservative
(pessimistic) measurement of the JIT's steady-state throughput. These
are single-host, single-session sanity numbers with no isolated
benchmarking environment (no CPU pinning/governor control, no
repeated-session averaging) — adequate to check the "must not lose" bar
below, not a rigorous benchmark suite.

Three workloads, each scaled to land around 1-2.5s of interpreter time
on this host:

- **(a) hot numeric loop** — a 4×10^8-iteration `sum` loop plus
  `fib(31)`, both pure double arithmetic with no property access.
- **(b) `stress.js` shapes, scaled** — the `arith`/`props`/`fib` bodies
  from `test/jit/x86-64/stress.js` (arithmetic, bitops, and the
  Point-with-getter property pattern together), run in an outer loop
  4000× plus `fib(30)`, since the file as committed runs each shape only
  a handful of times (it's a correctness differential, not a benchmark).
- **(c) property/IC-heavy loop** — the `Point`/`getX`/`.sum()`
  monomorphic-read pattern from `test/jit/x86-64/props.js`, scaled to 2×10^7
  iterations: constructor call, own-property read (object-specialization
  IC tier) and prototype-method read (parent-specialization IC tier)
  every iteration.

| Workload | Interpreter (median of 3) | `-Xjit=force` (median of 3) | Speedup |
|---|---|---|---|
| (a) numeric loop | 2.52 s | 1.12 s | **2.25×** |
| (b) stress.js shapes, scaled | 1.04 s | 0.65 s | **1.61×** |
| (c) property/IC loop | 1.44 s | 0.99 s | **1.45×** |

**Verdict: perf sane.** The JIT wins on all three; nowhere close to
losing to the interpreter, so the v1 bar is comfortably cleared. The
property/IC workload (c) shows the smallest margin, which is expected —
inline-cache dispatch and allocation dominate more of that workload's
time than raw arithmetic, and neither is JIT-accelerated as aggressively
as the arithmetic fast paths (Object allocation still goes through the
young-gen bump-allocator inline path, but the constructor call itself and
the `.sum()` prototype-method call are real calls, not inlined). No
tuning was attempted; a real perf pass (register-budget tradeoffs, the
xmm-globals question, thunk-vs-inline-call reconsideration) is explicitly
future work per the spec.

The perf scripts used are not checked into the tree (they aren't tests —
no assertions, just timing); reconstructing them from the descriptions
above is straightforward, or see the milestone-6 task-3 report for the
exact source.

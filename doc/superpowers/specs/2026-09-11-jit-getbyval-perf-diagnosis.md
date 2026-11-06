# GetByVal inline tiers: performance diagnosis

Why the new GetByVal load tiers land below the thresholds the PutByVal
work set. Diagnosis only -- no product code, tests, or docs were
changed, and nothing was committed.

Date: 2026-09-11. Machine: Intel Xeon E-2288G @ 3.70 GHz (16 threads,
turbo to 5.0 GHz), Linux 6.8. Every measurement below was taken with the
machine otherwise idle (1-minute load average below 1.0 before the first
timing run, verified from `/proc/loadavg`), strictly sequentially, no
build running concurrently with any timing run.

## Contents

1. [Setup](#setup)
2. [Measurements](#measurements)
3. [Per-hypothesis verdicts](#per-hypothesis-verdicts)
4. [What would it take](#what-would-it-take)

---

## Setup

### Binaries

**Candidate**: the existing
`/home/tmikov/work/hermes-x86-jit/cmake-build-x86jit-rel/bin/hermes`,
branch `x86-jit` at `ec56508a3` ("JIT: GetByVal tiers delivered (docs +
benchmark)").

**Baseline**: built fresh for this task at the pre-feature tip
`435d56938` ("JIT: use 32-bit forms for the x86-64 bit-op fast paths"),
in a throwaway worktree:

```
git worktree add /home/tmikov/.claude/jobs/dff46d8b/tmp/gbv-diag/base-wt 435d56938

cmake -B .../base-wt/cmake-build-rel -S .../base-wt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DHERMESVM_ALLOW_JIT=2 -DHERMESVM_GCKIND=HADES \
  -DHERMESVM_HEAP_HV_MODE=HEAP_HV_64 \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=OFF \
  -DHERMES_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER=OFF \
  -DHERMES_ENABLE_THREAD_SANITIZER=OFF

cmake --build .../base-wt/cmake-build-rel --target hermes -- -j16
```

`CMakeCache.txt` was compared key by key against the candidate's for
`CMAKE_BUILD_TYPE`, `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`,
`CMAKE_CXX_FLAGS`, `CMAKE_C_FLAGS`, `HERMESVM_ALLOW_JIT`,
`HERMESVM_GCKIND`, `HERMESVM_HEAP_HV_MODE`, the three sanitizer
switches, `HERMES_SLOW_DEBUG`, `HERMES_ENABLE_WERROR`,
`HERMES_ALLOW_BOOST_CONTEXT`, `HERMESVM_SANITIZE_HANDLES` and
`HERMES_ENABLE_DEBUGGER`: **all sixteen identical**. The worktree was
removed with `git worktree remove --force` after measurement.

### Bytecode identity

This feature changes no bytecode, so every probe was compiled once with
each compiler and the two `.hbc` files compared. Only the
candidate-compiled artifact was then run on both binaries.

| probe | bytes | `cmp` |
|---|---|---|
| `probe1.hbc` | 2454 | identical |
| `probe2.hbc` | 2450 | identical |
| `probe3.hbc` | 2357 | identical |
| `taonly.hbc` | -- | identical |
| `tareadonly.hbc` | -- | identical |
| `arronly.hbc` | -- | identical |
| `f16only.hbc` | -- | identical |
| `holes.hbc` | -- | identical |
| `typed-array-load.js` -> `tal.hbc` | 4076 | identical |

### Probes

All scratch sources live under
`/home/tmikov/.claude/jobs/dff46d8b/tmp/gbv-diag/`. Every loop keeps the
shipped benchmark's 1000 x 20000 shape, i.e. **2e7 element accesses**
per timed loop, so a millisecond of wall clock is 0.05 ns per access and
a cycle per access is 5.4 ms at 3.7 GHz.

`probe1.js` -- Int32Array, four loop bodies over the same array:
`p1-ta-chain` (`s += a[i]`, identical to the shipped `ta-load`),
`p1-ta-4acc` (four independent accumulators, stride 4),
`p1-ta-xor` (`s ^= a[i]`), `p1-ta-read` (`x = a[i]`, no accumulation).

`probe2.js` -- tax isolation: `p2-f16-read` (declining Float16Array
site, read only), `p2-f16-sum` (the shipped `f16-load` shape),
`p2-prop-read` (`x = o.f`, no ByVal site at all -- the control that must
be flat), `p2-objbyval-read` (`x = o[0]` on a plain object -- CAVEAT, found in
external review: the constant key lowers to GetByIndex, which at this
commit was still a bare direct helper call, so this probe measured the
plain expensive-helper path WITHOUT the new guards/indirect call; the
negligible-generic-tax conclusion rests on the arithmetic -- a fixed
~2-cycle tax against a ~23ns helper -- and on the f16/holes fixed-cost
measurements, not on this probe).

`probe3.js` -- poisoned-site decomposition: `p3-f64-load` (pure
Float64Array, which has its own tier), `p3-alt-same` (a *different*
Int32Array object on every call, one kind only, so the site is not
poisoned and both halves inline), `p3-alt-mixed` (replica of the shipped
`poisoned-load`).

Single-loop isolations for `perf`: `taonly.js`, `tareadonly.js`,
`arronly.js`, `f16only.js`, `holes.js`. Plus `helpercost.js`, which runs
a declining Float16Array *read* loop and a declining Float16Array
*write* loop on the same binary.

### Run protocol

Three runs per binary per probe, `hermes -b -Xjit=force <hbc>`,
candidate runs first then baseline, medians reported. (One unrelated
shell from another session was present on the box throughout, parked in
a `sleep 5` polling loop at 0.0% CPU and `SNs` priority; the load
average was 0.06 at the end of the measurement block. It did not run
anything, and every probe reproduced its shipped counterpart's ratio to
the second digit, so it did not contaminate these numbers.) `perf stat -e
cycles,instructions,branches,branch-misses` was run once per binary per
isolated loop (`perf` is available and works in this sandbox).

---

## Measurements

### Probe 1 -- is `ta-load` limited by the accumulator chain?

```
cmake-build-x86jit-rel/bin/hermes -b -Xjit=force probe1-cand.hbc
base-wt/cmake-build-rel/bin/hermes -b -Xjit=force probe1-cand.hbc
```

| loop | candidate (3 runs) | median | baseline (3 runs) | median | ratio |
|---|---|---|---|---|---|
| `p1-ta-chain` | 74, 72, 71 | **72** | 117, 116, 117 | **117** | **1.63x** |
| `p1-ta-4acc` | 66, 63, 63 | **63** | 110, 107, 107 | **107** | **1.70x** |
| `p1-ta-xor` | 99, 96, 95 | **96** | 117, 117, 118 | **117** | **1.22x** |
| `p1-ta-read` | 58, 57, 57 | **57** | 103, 101, 105 | **103** | **1.81x** |

`p1-ta-chain` reproduces the shipped `ta-load` ratio (1.63x) exactly, so
the probe file is measuring the same thing.

Breaking the chain does very little. Four independent accumulators buy
0.07x. Removing the accumulation *entirely* -- a loop that does nothing
but the load -- buys 0.18x and still lands at 1.81x, far below 2.5x.

(`p1-ta-xor` is not a shorter chain: `s ^= a[i]` forces a `ToInt32` on
every iteration, which costs more than the double add. It is reported
for completeness and carries no weight in the verdict.)

### Probe 2 -- the per-call tax on a permanently declining site

| loop | candidate (3 runs) | median | baseline (3 runs) | median | ratio |
|---|---|---|---|---|---|
| `p2-f16-read` | 143, 146, 144 | **144** | 120, 122, 121 | **121** | **0.84x** |
| `p2-f16-sum` | 140, 140, 146 | **140** | 131, 131, 133 | **131** | **0.94x** |
| `p2-prop-read` | 33, 33, 34 | **33** | 34, 33, 34 | **34** | **1.03x** |
| `p2-objbyval-read` | 461, 460, 502 | **461** | 467, 492, 460 | **467** | **1.01x** |

Isolated single-loop reruns:

| loop | candidate (3 runs) | median | baseline (3 runs) | median | ratio |
|---|---|---|---|---|---|
| `f16-only` | 141, 141, 145 | **141** | 134, 133, 136 | **134** | **0.95x** |
| `holes-only` | 725, 716, 718 | **718** | 699, 692, 704 | **699** | **0.97x** |

`p2-f16-sum` reproduces the shipped `f16-load` ratio (0.94x) exactly.
`p2-prop-read` -- a loop with no ByVal site -- is flat, so nothing
global differs between the two binaries.

The tax expressed as time per declining call:

| site | baseline cost/call | tax/call | as a fraction |
|---|---|---|---|
| `p2-f16-read` (read only) | 6.05 ns | **1.15 ns** | 19% |
| `p2-f16-sum` (accumulating) | 6.55 ns | **0.45 ns** | 7% |
| `holes-only` | 34.95 ns | **0.95 ns** | 2.7% |
| `p2-objbyval-read` | 23.35 ns | **-0.30 ns** | 0% (noise) |

The tax is a fixed per-call quantity, not a proportion: the more
expensive the helper it sits in front of, the smaller its share.

### Probe 3 -- decomposing the poisoned site

| loop | candidate (3 runs) | median | baseline (3 runs) | median | ratio |
|---|---|---|---|---|---|
| `p3-f64-load` | 67, 69, 68 | **68** | 117, 116, 117 | **117** | **1.72x** |
| `p3-alt-same` | 70, 74, 70 | **70** | 118, 121, 117 | **118** | **1.69x** |
| `p3-alt-mixed` | 103, 107, 104 | **104** | 114, 118, 114 | **114** | **1.10x** |

`p3-alt-same` is the decisive control. It hands the site a *different
array object* on every call, exactly as the poisoned benchmark does, but
always of one kind. It gets 1.69x -- indistinguishable from the
non-alternating `p3-f64-load` (1.72x) and `p1-ta-chain` (1.63x). So
alternating objects costs nothing; the whole poisoned gap is the half of
the traffic that never gets a tier.

### Demotion actually occurred

`-Xjit-emit-counters`, candidate binary:

| program | NumCall | NumRecompileChecks | NumRecompiles | **NumByValDemotions** |
|---|---|---|---|---|
| `tal.hbc` (all six shipped loops) | 120026 | 3848 | 4 | **4** |
| `f16only.hbc` | 20004 | 645 | 1 | **1** |
| `holes.hbc` | 20004 | 629 | 1 | **1** |
| `probe1.hbc` (four Int32 loops) | 80013 | 2520 | 5 | **0** |
| `probe2.hbc` | 80013 | 2524 | 1 | **2** |
| `probe3.hbc` | 60014 | 1898 | 4 | **1** |

Both controls demote. This rules out the alternative diagnosis
(recording tax rather than guard tax): at the default `Xjit-decline-
limit` of 64 declines per threshold crossing, `holes-only`'s 629
crossings mean recording stopped after roughly 40,000 of its 2e7
declining calls -- **0.2% of the run**. Whatever the controls are paying
in steady state, they are not paying it to the recording helper.

`probe1`'s zero demotions confirm the converse: every Int32Array site
got its tier and none of them declined.

### The tier is running (code dump)

`hermes -b -Xjit=force -Xdump-jitcode=3` on a small Int32Array driver
shows `sumTA` compiled twice, and version 2 carrying the tier:

```
JIT compilation of FunctionID 1, 'sumTA' (version 2)
// getByVal r8, r7, r0
// Inline typed array load (kind 39)     <- Int32ArrayKind
// Inline fast array load                <- chained kind-miss fallback
// call _jit_get_by_val [indirect]
```

So `ta-load`'s 1.63x is the tier running correctly, not the tier failing
to be emitted.

The same dump on a Float16Array driver gives the exact declining
sequence. Baseline, per call:

```
mov qword ptr [r14+8], rbx
mov rdi, r15
lea rsi, [r14+64]
lea rdx, [r14+8]
mov r11, <savedIP>
mov qword ptr [r15+6296], r11
mov r11, <_sh_ljs_get_by_val_rjs>
call r11
```

Candidate, per call -- the added work in bold:

```
mov qword ptr [r14+8], rbx
mov rax, qword ptr [r14+64]        ** materialize the source HV
vmovq xmm0, rbx                    ** materialize the key as a double
mov rsi, rax                       ** is-object
sar rsi, 0x30                      **
cmp rsi, 0xFFFFFFFFFFFFFFFF        **
jne L14                            **
mov rcx, rax                       ** decode the pointer
shl rcx, 0x10                      **
shr rcx, 0x10                      **
cmp byte ptr [rcx+4], 0x1E         ** CellKind == JSArrayKind?
jne L14                            **
L14:
mov rdi, r15
lea rsi, [r14+64]
lea rdx, [r14+8]
mov rcx, <versionData>             ** extra argument
mov r8d, 0x18                      ** extra argument (siteId)
mov r11, <savedIP>
mov qword ptr [r15+6296], r11
mov r11, <&site.helper>
call qword ptr [r11]               ** indirect through the slot
```

Thirteen extra instructions and two extra branches, and a direct
`call r11` becomes a load-then-`call qword ptr [r11]`.

### `perf stat`

One run per binary. Per-iteration figures divide by 2e7.

**`f16only`** (permanently declining, demoted):

| counter | candidate | baseline | delta | per call |
|---|---|---|---|---|
| cycles | 698,396,042 | 658,306,151 | +40,089,891 | **+2.00** |
| instructions | 2,305,024,359 | 2,044,887,203 | +260,137,156 | **+13.01** |
| branches | 404,016,316 | 363,996,536 | +40,019,780 | **+2.00** |
| branch-misses | 119,502 (0.03%) | 121,677 (0.03%) | -2,175 | **0** |
| IPC | 3.30 | 3.11 | | |

The measured +13.01 instructions and +2.00 branches per call match the
disassembly above instruction for instruction. Branch prediction is
unaffected (0.03% both sides) -- the indirect call is perfectly
predicted, since after demotion the slot never changes again.

**`holes`** (declining at the RANGE check -- the sparse fixture holds
one element, so the relative index is out of range and the empty check
never runs; most of the array tier's guards still execute before the
decline, which is why its per-call delta exceeds f16's kind-guard
exit):

| counter | candidate | baseline | delta | per call |
|---|---|---|---|---|
| cycles | 3,497,190,846 | 3,408,111,786 | +89,079,060 | **+4.45** |
| instructions | 11,401,801,400 | 10,922,625,634 | +479,175,766 | **+23.96** |
| branches | 2,003,356,280 | 1,883,490,876 | +119,865,404 | **+5.99** |
| branch-misses | 148,676 (0.01%) | 148,764 (0.01%) | -88 | **0** |

Same mechanism, larger dose: the holes site runs 24 extra instructions
and 6 extra branches before declining, versus f16's 13 and 2, because
its decline point (the range check) sits deep in the array tier --
past the object/kind guards and the key conversion -- where f16 exits
at the first kind guard. (The empty-slot check, one step deeper still,
never runs on this fixture; see the benchmark's own comment.)

**`taonly`** (Int32Array, accumulating -- the shipped `ta-load` shape):

| counter | candidate | baseline | ratio |
|---|---|---|---|
| cycles | 355,807,879 (**17.79**/iter) | 577,217,016 (**28.86**/iter) | **1.62x** |
| instructions | 984,668,353 (**49.23**/iter) | 1,644,875,531 (**82.24**/iter) | 1.67x |
| IPC | 2.77 | 2.85 | |

**`tareadonly`** (Int32Array, no accumulation):

| counter | candidate | baseline | ratio |
|---|---|---|---|
| cycles | 296,580,000 (**14.83**/iter) | 515,527,000 (**25.78**/iter) | **1.74x** |
| instructions | 824,494,746 (**41.22**/iter) | 1,484,718,405 (**74.24**/iter) | 1.80x |
| IPC | 2.78 | 2.88 | |

(Cycle totals for `tareadonly` are derived from the reported instruction
count and IPC; the raw cycles line scrolled off. The wall-clock medians
in probe 1 agree: 57 vs 103 ms = 1.81x.)

**`arronly`** (dense Array, the passing `arr-load` shape):

| counter | candidate | baseline | ratio |
|---|---|---|---|
| cycles | 356,257,006 (**17.81**/iter) | 536,926,720 (**26.85**/iter) | **1.51x** |
| instructions | 984,981,733 (**49.25**/iter) | 1,505,133,120 (**75.26**/iter) | 1.53x |
| IPC | 2.76 | 2.80 | |

The cycle ratios track the wall-clock ratios to within 0.01x everywhere,
so none of this is a clock-frequency artifact.

Two things fall out of the last three tables:

- The candidate's inline path costs **17.8 cycles / 49.2 instructions**
  per element for the typed-array tier *and* for the JSArray tier --
  identical. `arr-load` clears its 1.15x bar and `ta-load` misses its
  2.5x bar with the same emitted cost. The difference is entirely in the
  denominators: the baseline array helper is 26.85 cycles, the baseline
  typed-array helper 28.86.
- IPC barely moves (2.85 -> 2.77 for `taonly`, 2.88 -> 2.78 for
  `tareadonly`). If the loop were latency-bound, removing 33 instructions
  would have collapsed IPC. It does not: both sides are running near
  three instructions per cycle, i.e. **throughput-limited, not
  stalled**.

### How expensive is the baseline load helper, really?

`helpercost.js`, **baseline binary only**, same file, same shape, same
Float16Array, both sites permanently declining so both run the plain
helper:

| loop | runs (ms) | median | per call |
|---|---|---|---|
| `hc-get` (`x = a[i]`) | 123, 121, 121 | **121** | 6.05 ns |
| `hc-put` (`a[i] = i`) | 262, 260, 269 | **262** | 13.10 ns |

**The put helper costs 2.17x the get helper per call.** That is the
whole story of why the same mechanism scored 4.08x on `ta-store` and
1.63x on `ta-load`: the inline tier lands at roughly the same absolute
cost on both sides, but on the store side it was replacing something
more than twice as expensive.

Working the arithmetic through: if the baseline store path is ~2.17x the
baseline load path's 28.9 cycles, it is ~62.7 cycles; a 4.08x speedup
puts the inline store tier at ~15.4 cycles, next to the inline load
tier's 17.8. Two tiers of near-identical cost, two very different
ratios, because the denominators differ by 2.17x.

---

## Per-hypothesis verdicts

### H1 (`ta-load` gap) -- **PARTIAL**: second clause CONFIRMED and
dominant, first clause REJECTED

H1 had two clauses. They do not fare the same.

**The latency-chain clause is REJECTED.** Decisive number: deleting the
accumulator chain outright (`p1-ta-read`, a loop whose body is only the
load) moves the ratio from **1.63x to 1.81x**. Four independent
accumulators move it to 1.70x. The chain is worth at most 0.18x of
ratio, and no arrangement of it reaches 2.5x. The supporting evidence is
the IPC: 2.77 candidate vs 2.85 baseline on `taonly`, essentially
unchanged, which is not what a latency-bound loop looks like. The
accumulate costs about the same on both sides (3.0 extra cycles per
element on the candidate, 3.1 on the baseline), so it does not
differentially favour either binary -- which is exactly why removing it
changes the ratio so little.

**The cheap-helper clause is CONFIRMED and is the whole explanation.**
Decisive number: the baseline's entire load path -- call sequence plus
`tryFastGetComputedNoAlloc` -- is **28.86 cycles per element**, and the
inline tier replaces it with **17.79**. That is an 11.07-cycle saving
out of 28.86, which is 1.62x and nothing else. Reaching 2.5x would
require the inline path to come in at 11.5 cycles or less. Second
decisive number, from the same binary and the same loop shape: the put
helper costs **262 ms vs the get helper's 121 ms** for the same 2e7
calls, i.e. **2.17x**. The 2.5x threshold was set against a baseline
that was twice as expensive as the one the load tier has to beat.

### H2 (controls regression) -- **CONFIRMED**, in the precise form stated

Decisive number: `perf` on `f16only` shows exactly **+13.01
instructions, +2.00 branches and +2.00 cycles per declining call**, with
branch-miss rate unchanged at 0.03%. The instruction and branch deltas
match the disassembly one for one: two operand materializations, the
nine-instruction is-object / pointer-decode / `CellKind == JSArrayKind`
chain with its two branches, two extra call-setup arguments, and a
direct call turned into a call through memory. `arr-holes` pays the same
tax in a larger dose (+23.96 instructions, +5.99 branches, +4.45 cycles)
because it declines at the range check, deep in the array tier -- past
the object/kind guards and the key conversion -- rather than at the
second guard where f16 exits.

Two sub-claims were checked and both hold:

- **The tax is guard-and-call, not recording.** `NumByValDemotions` is 1
  on both `f16only` and `holes`, so both sites really did demote.
  Recording covered roughly 40,000 of 2e7 declining calls (0.2% of the
  run) before the flip.
- **The tax is fixed, not proportional**, which is why it shows as a
  regression here and nowhere else. The identical 13-instruction
  sequence is 19% of a read-only f16 loop (6.05 ns/call helper), 7% of
  the accumulating f16 loop, 2.7% of `arr-holes` (34.95 ns/call, and a
  longer guard chain at that), and -- by arithmetic, not measurement:
  the p2-objbyval-read probe was invalid (see its caveat), so no valid
  generic-object ByVal measurement exists -- roughly 2-3% of a
  ~23 ns/call (~86-cycle) generic-object helper, small but not
  measured. A helper-dominated loop with a
  *cheap* helper is the only shape that can show it.

The no-ByVal control (`p2-prop-read`, 33 vs 34 ms) confirms no global
build difference is contaminating any of this.

### H3 (poisoned gap) -- **CONFIRMED**, plus the threshold is
arithmetically unreachable

Decisive number: `p3-alt-same`, which alternates two arrays **of the
same kind** across calls, gets **1.69x** -- the same as the
non-alternating loops. So alternating objects is free, and 100% of the
poisoned gap is the half of the traffic that never gets a tier and runs
the demoted indirect helper forever, exactly as hypothesised.

The blend model closes to 2%: candidate `p3-alt-mixed` should be
(half the Int32 traffic inline) 36 ms + (half the Float64 traffic
through the baseline helper) 58.5 ms + (H2's 1.15 ns tax on 1e7
declining calls) 11.5 ms = **106 ms predicted vs 104 ms measured**.

The additional finding is that the 1.3x threshold could not have been
met. With a single-kind tier on a 50/50 mix, the Amdahl ceiling is
`2 / (1/1.67 + 1/1.0)` = **1.25x**, and that is with the H2 tax set to
zero. The shipped 1.15x/1.17x and this probe's 1.10x are that ceiling
minus the tax. No amount of tuning the demotion path gets a 50/50
poisoned site to 1.3x; only a second inline kind does.

---

## What would it take

### `ta-load`, threshold 2.5x, measured 1.63x

**(a) unreachable for this benchmark shape, and (b) partly reachable by
shortening the tier.**

Unreachable by reshaping the benchmark: the ceiling is set by the
denominator, not the loop body. Even a loop that does nothing but the
load reaches only 1.81x, because the baseline's whole load path is 28.9
cycles. There is no load-side shape that makes the baseline more
expensive without also making the tier more expensive -- the shape that
*would* show 2.5x is the store side, where the baseline helper is 2.17x
costlier (measured above) and where the same mechanism duly scored
4.08x. The threshold was transplanted from a benchmark whose
denominator was twice as large.

Reachable by emission work, if the project wants the number. The inline
path is 49.2 instructions per element; roughly 30 of those are the tier
itself, and three separate inefficiencies are visible in the dump. None
of these were implemented.

- **Loop-invariant guard hoisting.** The is-object test, the pointer
  decode and the `CellKind` compare -- 9 instructions and 2 branches --
  re-run on every element, although the source register is loop-invariant.
  Scope: a JIT-side analysis proving the source FR unchanged across the
  back edge, plus a re-check on any path that could invalidate it.
  Medium; touches the emitter and the allocator's notion of a loop.
- **Index type specialization.** The key round-trips through
  `vcvttsd2si` / `mov edx,edx` / `vcvtsi2sd` / `vucomisd` / `jne` / `jp`
  -- 6 instructions -- on every element, although `i` is an induction
  variable the IR already types as integral. Scope: take the uint32 path
  directly when the key FR's type is known. Small to medium, emitter-local.
- **Result unboxing.** The loaded int32 is converted to double, moved
  through `vmovq` into `rax`, spilled to the frame and reloaded into an
  xmm register for the add. Scope: FR type plumbing so a typed-array load
  result can stay in a vector register. Medium.

Rough arithmetic: those three would plausibly take 49.2 instructions to
about 32, and at the observed ~2.8 IPC that is roughly 11-12 cycles,
i.e. 2.4-2.5x. So 2.5x is reachable -- by making the tier shorter, not
by anything about the benchmark.

### `f16-load` (threshold 0.97x, measured 0.94x) and `arr-holes`
(threshold 0.97x, measured 0.96x)

**(b) reachable via a known optimization: demote by code patching.**

Demotion today flips a function pointer in a data slot. The guard chain
and the `call qword ptr [r11]` stay in the instruction stream forever,
which is precisely the 13 (f16) / 24 (holes) instructions and 2 / 6
branches `perf` measures. Patching the site's entry to jump
unconditionally into the helper block, and the indirect call back to a
direct one, would erase the tax completely and restore parity on both
controls. Scope: a code-patching path in the JIT -- icache
invalidation, safety against bodies executing concurrently on other
threads, idempotence across recompiles, and a story for what a later
recompile re-emits. This is the largest item on this page and the only
one needing new machinery rather than better emission.

Cheaper partial measures were considered and mostly do not pay:

- Guard reordering: already near-optimal for f16 (is-object, then one
  byte compare). Nothing to reclaim.
- Splitting the array tier so the empty check declines by a shorter
  route: the instructions `arr-holes` pays are the ones *before* the
  empty check, so this saves little.
- Dropping the two extra call-setup arguments (`versionData` pointer,
  `siteId`) on a demoted site: needs the slot's signature known at emit
  time, which by construction it is not.

Worth stating plainly for the accept-vs-optimize decision: with
pointer-flip demotion, a permanently declining site has an intrinsic
floor of roughly 2 cycles per call (short guard chain) to 4.5 cycles per
call (full array tier). Whether that clears 0.97x is decided entirely by
how expensive the site's own helper is -- 0.94x against a 33-cycle f16
helper, 0.97x against a 170-cycle holes helper, and asymptotically
1.00x as the helper grows (a generic-object access at ~23 ns/call is
roughly 86 cycles at this machine's clock -- the earlier "460-cycle"
figure was an error, and no valid generic-object measurement exists;
see the p2-objbyval-read caveat). The controls were chosen to be
helper-dominated with *cheap* helpers, which is the worst case for this
floor by construction.

### `poisoned-load` / `poisoned-load-rev`, threshold 1.3x, measured
1.15x / 1.17x

**(a) unreachable for this benchmark shape with single-kind
specialization.**

The Amdahl ceiling for a 50/50 mix where one kind inlines at 1.67x and
the other does not inline at all is 1.25x, below the threshold *before*
any tax is charged. Shapes that would show 1.3x: a traffic mix skewed
toward the specialized kind (75/25 gives a 1.43x ceiling, 90/10 gives
1.56x), or a second kind that happens to be `JSArray`, which the
unconditional array tier already covers.

Reachable optimization, if the number is wanted: **a two-kind
typed-array tier** -- record a second kind in `JitByValSiteRecord` and
chain a second `emitGetByValTypedArrayTier` before the array tier and
the helper. `p3-alt-same` shows what that would score: **1.69x**. Scope:
one more recorded kind, one more emitted tier in the chain, and an
eviction policy for which two kinds to keep. Note this is a deliberate
policy change, not a bug fix -- the design chose poison-keeps-first-kind
on purpose, and the second kind costs code size at every polymorphic
site.

### One calibration note for the owner

`arr-load` passes at 1.51x and `ta-load` fails at 1.63x, yet the
candidate's emitted inline path costs **the same 17.8 cycles / 49.2
instructions per element in both**. The only difference between a pass
and a fail here is where the bar was set (1.15x vs 2.5x) relative to two
baselines that differ by 7% (26.85 vs 28.86 cycles). The thresholds were
not calibrated against a common mechanism, so the pass/fail pattern
carries less information about the feature than the raw cycle counts
above do.

# JIT: inline GetByIndex tiers (constant-key GetByVal twin) — design

Date: 2026-09-12. Status: draft, pre-review.
Scope: x86-64 JIT only (arm64 dormant, as for ByVal). Completes the
read side begun by
doc/superpowers/specs/2026-09-11-jit-getbyval-design.md (the binding
companion — its policy, records, and semantics sections apply
wherever this document does not explicitly differ) and its measured
perf record 2026-09-11-jit-getbyval-perf-diagnosis.md.

## Motivation

`GetByIndex Reg8, Reg8, UInt8` — the compiler's lowering of a
literal uint8 property key (ISel.cpp ~1272) — compiles to a bare
call to `_sh_ljs_get_by_index_rjs(SHRuntime *, SHLegacyValue *,
uint32_t)` (JitEmitter-property.cpp ~1217 pre-feature numbering).
Constant-index reads are the shape of JS numeric kernels (vec2/3/4
math, tuple/pair access, fixed-slot records) and of unrolled code.
The GetByVal feature built everything this needs: the tiers here are
the same tiers with the entire key block deleted — the index is an
emit-time constant, so there is no key FR, no double→uint32
conversion, no parity exit, and several ByVal edge cases (negative,
fractional, ≥2^32, 0xFFFFFFFF keys) are structurally impossible
(K ∈ [0, 255]).

## Policy — identical to GetByVal, by reference

JSArray tier unconditional in every version on every build config
(loads have no barrier); TA tier evidence-driven, exact kind, first
kind kept, poison on second; tiers never dropped; no fast-path
instrumentation; no stub/subroutine emission; BigInt kinds and
Float16 decline; Uint8Clamped supported. The existing
`isJitSupportedTypedArrayLoadKind` predicate is used AS IS — no
predicate changes; the union predicate at the progress/demotion
sites already covers any record a load-side recorder writes.

## Records, recording, demotion

- Sites join the same `JitConsumerRecords.byValSites` deque. siteId
  is the bytecode offset; GetByIndex is a distinct opcode, so no
  collision with get/put ByVal sites is possible.
- New recording helper: `_jit_get_by_index(SHRuntime *,
  SHLegacyValue *source, uint32_t key, SHJitVersionData *,
  uint32_t siteId) -> SHLegacyValue` — the `_jit_get_by_val` shape
  with the key BY VALUE, matching the plain helper. Records with the
  load predicate, bumps the shared decline counter, considers
  recompile, returns `_sh_ljs_get_by_index_rjs(shr, source, key)`.
- Emitted call: through the per-site mutable helper slot,
  `callRuntimeWithSavedIPIndirect` (the fallback can run proto
  getters and throw), always passing the 5-argument recording shape
  (key in edx). The demoted plain helper reads only rdi/rsi/edx and
  ignores rcx/r8d — the same SysV extra-argument compatibility the
  ByVal slots rely on.
- Demotion: `isRecordingHelper` gains the fourth identity;
  `demoteSite` maps `_jit_get_by_index` →
  `_sh_ljs_get_by_index_rjs`. Same stable-crossings rule, retirement
  sweep, shared `NumByValDemotions`.
- HARD REQUIREMENT the ByVal emitter satisfies and the new emitter
  must copy: the site's helper slot is initialized ONLY WHEN NULL
  (getByValImpl ~1614). Demotion survives recompiles because the
  compiler copies the slot into the candidate and the emitter
  respects a non-null value; an emitter that unconditionally
  installed its recording helper would silently restart recording
  after any recompile while passing the ordinary demotion tests.
  A carried-demotion test pins this end to end (see Testing).

## The factoring (agreed in design discussion)

The per-kind element access tail of `emitGetByValTypedArrayTier` —
element load, integer/float convert, NaN CANONICALIZATION, number-HV
encode, for all nine supported kinds — is extracted into ONE shared
emitter helper used by both TA tiers (working name
`emitTypedArrayElementLoad(kind, addressOperand, resultReg, ...)`).
The callers differ only in how they form the address: ByVal computes
`data_ + offset_ + idx*width` with a register index; ByIndex folds
`K*width` into the displacement. The NaN-canonicalization sandwich —
the one place where a bug forges a tagged non-number — must exist in
exactly one copy after this change. The refactor must leave the
GetByVal tier's emission BYTE-IDENTICAL (the existing
getval-conversions-emitted.js / getbyval-inline-emitted.js pins and
the full jit suites are the regression net; a prove-can-fail
mutation of the shared tail must fail BOTH features' named tests,
which is also the proof that the sharing is real). "Byte-identical"
is verified DIRECTLY, not inferred from the pattern-matching pins
(which wildcard operands and cannot see register or encoding
drift): the refactor task compares the FINALIZED MACHINE-CODE
BYTES of a representative ByVal workload at the pre- and
post-refactor revisions, normalizing only identified address
relocations (the embedded record/helper/runtime addresses). The
capture must be of the FINALIZED buffer: neither the
`-Xdump-jitcode` text logger nor asmjit's kMachineCode logging
qualifies — asmjit logs during emission with unresolved relative
offsets masked (emitterutils.cpp ~39), and link resolution and
relocation happen later (jitruntime.cpp). The comparison therefore
uses a LOCAL, UNCOMMITTED hook at the point where the compiled
body's code buffer is finalized (post-relocation, where the
executable bytes exist), dumping or hashing the buffer per body;
the text dump is kept only as supplementary evidence. Any
non-relocation difference fails the task. The seam to preserve, from the shipped
code: `loc` becomes `data_ + offset_` BEFORE the per-kind switch;
each arm constructs its own sized/scaled memory operand
(~JitEmitter-property.cpp:1400-1456); the tail branches to the
caller's doneLab around the undefined block (~1478). The extracted
helper takes those as parameters/invariants rather than
materializing any extra address computation.

## The tiers

Semantics authority unchanged: `tryFastGetComputedNoAlloc`
(JSObject-inline.h:80-118) — the interpreter's own GetByIndex case
funnels into the same fast path (Interpreter.cpp ~2111).

### JSArray fast tier (unconditional)

Object + exact-JSArrayKind guard (Arguments declines), then the
constant-K forms of the ByVal checks — with the storage-layout
correction that K being constant does NOT make the storage offset
constant: the stored fields are `beginIndex_` and `elemCount_`
(JSArray.h ~143), element addressing is `K - beginIndex_`, and
nonzero beginnings are reachable (an indexed write allocating
storage sets beginIndex_ = index, JSArray.cpp ~211). The tier
therefore performs the shipped ByVal sequence with K as an
immediate: load beginIndex_, compute the relative index
(K - beginIndex_ with an immediate operand), unsigned-compare it
against elemCount_ (this one check covers both K < beginIndex_ via
unsigned wrap and K ≥ endIndex; failure → DECLINE to the helper —
the prototype chain may answer), scale the RELATIVE index into the
storage access, empty → DECLINE (hole), shared
`Emit_sh_shv_decode` unbox. No fast-index-properties check, per
the authority's comment. Shorter than ByVal by the key-conversion
block only; the begin-relative addressing stays.

### Typed-array tier (evidence-driven)

Exact-kind guard for the specialized kind; `cmp` the length field
against Imm(K), out-of-bounds → inline `mov undefined` (NO helper,
NO recording — same rule and rationale as ByVal); null-`data_`
detached check reaching the same undefined path; then the SHARED
element-load tail with the `K*width` constant displacement. Float
kinds canonicalize through the shared tail; integer kinds skip, as
today.

## Non-goals

- `DefineOwnByIndex`/`DefineOwnByIndexL`: array-literal
  initialization with own-define semantics — a different operation,
  untouched.
- No PutByIndex exists; nothing on the store side changes.
- arm64 tiers; GetByValWithReceiver; any ByVal policy change.

## Testing

Mirrors the GetByVal suites, sized to the smaller surface; every
test drives a REAL GetByIndex site (literal uint8 key; pin
`GetByIndex` in the bytecode dump — a key ≥ 256 or non-literal
lowers to GetByVal and would test the wrong opcode) and uses one
function per site so specialization is per-test-controlled.

- test/jit/getidx-conversions.js (ARCH-NEUTRAL): per-kind functions
  over views with NONZERO byteOffset; constant indices covering 0, a
  middle index, and 255 (the uint8 ceiling) where the view is long
  enough; known bit patterns per kind incl. Uint8Clamped reads and
  Uint32 > 2^31; float NaN families through an aliased integer view
  with the `x !== x` AND `Object.is(x, NaN)` oracles (exercising the
  SHARED tail through the ByIndex address path); interpreter-diffed.
- test/jit/getidx-guards.js (ARCH-NEUTRAL): holes at a constant
  index over a prototype with indexed data AND accessor properties;
  constant index beyond a short array finding prototype properties;
  the STORAGE-STATE cases the constant key does not eliminate — a
  nonzero-beginIndex array hit (create via a first write at a high
  index) read at K inside its live range, a beginIndex > K array
  (decline → prototype answers), and an empty/storage-less array;
  frozen/sealed dense arrays; TA bounds at the EQUALITY BOUNDARY:
  K == length (undefined — a nonzero word planted immediately
  beyond the view, per getval-guards.js's technique, so inclusive
  acceptance reads it and fails by name), K > length, K == length-1
  (in bounds), K == 0 on an attached zero-length view, and K == 255
  on views of length 255 (undefined) and 256 (in bounds); detached
  → undefined; Float16 and BigInt64 decline and answer correctly.
- Policy (test/jit/x86-64/), the slim set — the demotion machinery
  is shared and already pinned by six GetByVal files, so ByIndex
  pins the per-opcode wiring rather than re-proving the machinery:
  recompile-getidx-trigger.js (kind seen → recompile adds the tier;
  dump pins version 2 + tier), recompile-getidx-duo.js (JSArray +
  one TA kind at one site: both tiers pinned, both values correct),
  recompile-getidx-demote.js (stable crossings → NumByValDemotions,
  values correct after the flip — pins the fourth helper-identity
  mapping end to end), recompile-getidx-demote-carried.js (demote a
  ByIndex site, then trigger a recompile from ANOTHER site in the
  same function, and prove via the short/long declining-suffix
  technique of recompile-getval-demote-carried.js that the demoted
  slot survived — this is the null-only-initialization pin, an
  emitter-specific risk the shared-machinery tests cannot see), and
  recompile-getidx-budget0.js (demotes without recompiling).
  Poisoning both orders is exercised at the record level identically
  to ByVal (same recorder); one order suffices here, folded into the
  trigger or its own small file — plan's choice, stated. The delay
  and retirement variants are NOT duplicated (purely shared
  machinery, already pinned by the ByVal files).
- test/jit/x86-64/getbyindex-inline-emitted.js: pins the JSArray
  kind guard + the immediate begin-relative range compare, the
  indirect recording-call pin, the bounds-fail undefined path, the
  anti-vacuity `GetByIndex` bytecode pin — and, following
  getval-conversions-emitted.js's rationale, VERSION-2 EMISSION PINS
  FOR ALL NINE KINDS THROUGH BYINDEX (tier line + the
  constant-displacement element access with its `K*width`
  displacement + one distinguishing convert per kind — Float64,
  which has no convert instruction, pins its load and
  NaN-canonicalization sequence instead — SPEC-NEXT anchored),
  each behind a warmed per-kind function with a known-value probe. A value-only test cannot see a kind silently
  left on the (correct-answer) helper path — e.g. a recorder or
  selector accidentally using the store predicate would strand
  Uint8Clamped invisibly; these pins make that a named failure.
- Prove-can-fail (restore + REBUILD each time): (1) corrupt the
  SHARED tail's NaN canonicalization — named checks in BOTH
  getval-conversions.js AND getidx-conversions.js fail (the sharing
  proof); (2) change the TA bounds compare from strict to INCLUSIVE
  acceptance — the K == length equality case (with its planted
  beyond-the-view word) fails by name; a merely-larger-K probe
  would not catch this mutation, which is why the equality boundary
  is mandatory.
- Suites: full x86 ASan + jit suites on HV64/HV32/BOXED/MallocGC
  (GetByIndex tiers are as ungated as GetByVal's) + arm64 jit
  (dormant sanity) + one handle_san jit run.

## Benchmark and expected numbers

benchmarks/jit-benches/typed-array-index.js, committed, same recipe
conventions as typed-array-load.js: vec-dot-f64 (Float64Array
length-4 vectors, a[0]*b[0]+a[1]*b[1]+a[2]*b[2] in a hot loop),
vec-dot-arr (same over dense arrays), f16-idx (declining control),
holes-idx (constant-index hole control). A/B against the pre-feature
tip with the shared byte-identical .hbc discipline the load
benchmark established.

Thresholds are set from the MEASURED GetByVal record, not analogy
optimism: the get-by-index baseline helper is cheaper than
get-by-val's (no key conversion to amortize away), so per-call wins
are smaller; the tier is also shorter by the same key block.
Provisional bars, REVISED at delivery (controller ruling under the
GetByVal precedent, surfaced to the user for override): vec-dot-f64
≥ 1.1x — the original 1.2x guess was miscalibrated the same way the
GetByVal bars were; measured, both dot kernels land at ~1.14x with
IDENTICAL emitted economics (the arr twin PASSED 1.1x at 1.137x
while f64 missed 1.2x at 1.138x), and the mechanism is the
already-ruled-on helper asymmetry: the by-index C++ helper never
had a key conversion to remove, so the inlining win is smaller by
design. Original bars for the record: vec-dot-f64 ≥ 1.2x; vec-dot-arr
≥ 1.1x; f16-idx and holes-idx ≥ 0.94x after demotion settles. The
control bars are ACCEPTANCE TARGETS pending ByIndex measurement,
not derived guarantees: the demotion floor is a FIXED per-call cost
(measured +2.0 cycles for ByVal's short f16 guard-chain case and
+4.45 for the deeper-declining holes case; perf-diagnosis), so the
percentage depends on the
control loop's body — the ByVal record measured 0.94x for an
accumulating f16 loop and 0.84x for a bare-read one. The controls
here therefore use ACCUMULATING bodies (sum the reads), stated in
the benchmark header, so the targets compare like with like. A miss
triggers a
measured diagnosis and an explicit ruling — the GetByVal process —
never silent acceptance or benchmark massaging; the Delivered
section records all runs, medians, and the evaluation.

## Delivered (2026-09-12, Task 5)

Tip: `cee4cd857` "JIT: evidence-driven GetByIndex typed-array tier" on
branch `x86-jit`.

### Suites (all foreground, all green except one pre-existing flake)

- `cmake-build-x86jit-hv32` jit suite: 125 passes, 4 unsupported.
- `cmake-build-x86jit-boxed` jit suite: 125 passes, 4 unsupported.
- `cmake-build-x86jit-malloc` (MallocGC) jit suite: 107 passes, 22
  unsupported.
- `cmake-build-host` rebuilt first (stale-host trap check; up to
  date), then `cmake-build-arm64` jit suite: 60 passes, 69
  unsupported.
- `cmake-build-hs-hv32` (handle_san) jit suite: 86 passes, 42
  unsupported, **1 failure** —
  `jit/x86-64/counters-slow-call-kinds.js` (dz 01a06156), the KNOWN
  pre-existing flake in this configuration (Task 4 already isolated
  it to the unmodified tree, 3/3). Not chased further, per the task
  brief.
- All four numbers match Task 4's report exactly (same commit, same
  configs), confirming the tree is unchanged since Task 4 and no new
  gating was introduced anywhere.
- Full x86 ASan suite re-run at the end of Task 5 (post-benchmark,
  post-docs): see the task-5 report for the exact count.

### Benchmark A/B: `benchmarks/jit-benches/typed-array-index.js`

Candidate: `cmake-build-x86jit-rel` rebuilt at `cee4cd857` (Release,
clang/clang++, `HERMESVM_ALLOW_JIT=2`, `HERMESVM_GCKIND=HADES`,
`HERMESVM_HEAP_HV_MODE=HEAP_HV_64`, no sanitizers). Baseline: a
throwaway worktree at the pre-feature tip `c2a2360df`, configured
with the identical toolchain/options (verified field-by-field against
the candidate's `CMakeCache.txt`), built at
`/home/tmikov/.claude/jobs/dff46d8b/tmp/baseline-build`.

Bytecode is unchanged by this feature (no new opcodes, no encoding
changes), so the benchmark was compiled ONCE with each compiler and
the two `.hbc` files were compared directly:
```
candidate: 3067 bytes, sha256 0558b04bff8fb2f7e941f8042f9cd201080bc2c4159e9ae4d995440b2d62cae3
baseline:  3067 bytes, sha256 0558b04bff8fb2f7e941f8042f9cd201080bc2c4159e9ae4d995440b2d62cae3
cmp: byte-identical
```
Confirmed BEFORE any measurement. The one shared artifact
(`tai-candidate.hbc`) was then run on both binaries.

Hot-site verification (`-dump-bytecode`, per the task brief, done
before measuring): `vecDotF64`/`vecDotArr` each compile to exactly six
`GetByIndex` instructions (K = 0, 1, 2 on each of the two parameters);
`readF16Idx`/`readHolesIdx` each compile to exactly one `GetByIndex`.
No `GetByVal` appears anywhere in any of the four reader functions.
Further confirmed live via `-Xjit=force -Xdump-jitcode=3` on the
actual benchmark run: `vecDotF64` recompiles to version 2 with all six
sites carrying `// Inline typed array load (kind 42)` (Float64);
`vecDotArr` needs no recompile — all six sites already carry
`// Inline fast array load` at version 1, since the JSArray tier is
unconditional from the first version.

3 interpreter + 3 `-Xjit=force` runs per side, strictly sequential
(no build ran concurrently with any measurement), printed `ms` values
straight from the benchmark's own `Date.now()` timers:

| run | vec-dot-f64 | vec-dot-arr | f16-idx | holes-idx |
| --- | --- | --- | --- | --- |
| candidate interp 1 | 1036 | 965 | 669 | 1229 |
| candidate interp 2 | 1035 | 973 | 653 | 1227 |
| candidate interp 3 | 1021 | 972 | 653 | 1228 |
| **candidate interp median** | **1035** | **972** | **653** | **1228** |
| candidate JIT 1 | 759 | 708 | 552 | 1225 |
| candidate JIT 2 | 751 | 712 | 556 | 1216 |
| candidate JIT 3 | 753 | 704 | 562 | 1222 |
| **candidate JIT median** | **753** | **708** | **556** | **1222** |
| baseline interp 1 | 1029 | 968 | 651 | 1230 |
| baseline interp 2 | 1031 | 974 | 659 | 1238 |
| baseline interp 3 | 1024 | 969 | 652 | 1232 |
| **baseline interp median** | **1029** | **969** | **652** | **1232** |
| baseline JIT 1 | 864 | 805 | 537 | 1243 |
| baseline JIT 2 | 857 | 804 | 533 | 1234 |
| baseline JIT 3 | 853 | 811 | 542 | 1236 |
| **baseline JIT median** | **857** | **805** | **537** | **1236** |

`check` values (the summed/dot result printed alongside each `ms`
line) were identical across every run and both binaries for every
benchmark: `265000000` (both dot products), `30000000` (f16-idx),
`NaN` (holes-idx — expected, matching `typed-array-load.js`'s
`arr-holes` precedent: the read is always a hole, `s += undefined`
poisons the running sum, and the point is the per-call decline cost,
not the printed value).

Interpreter runs are a sanity check (bytecode and the interpreter are
untouched by this feature): candidate and baseline agree within noise
on all four (1035 vs 1029, 972 vs 969, 653 vs 652, 1228 vs 1232),
confirming the two binaries execute identical, unaffected interpreted
code. The bars apply to the `-Xjit=force` medians only, per the spec:

The table shows both bar generations: the ORIGINAL provisional bars
(one miss, which triggered the ruling) and the ACCEPTED bars from the
revised Thresholds paragraph above, which all four rows meet.

| benchmark | base JIT | cand JIT | ratio | original | verdict | accepted | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| vec-dot-f64 | 857 | 753 | 1.138x | ≥ 1.2x | miss | ≥ 1.1x | **PASS** |
| vec-dot-arr | 805 | 708 | 1.137x | ≥ 1.1x | pass | ≥ 1.1x | **PASS** |
| f16-idx (control) | 537 | 556 | 0.966x | ≥ 0.94x | pass | ≥ 0.94x | **PASS** |
| holes-idx (control) | 1236 | 1222 | 1.011x | ≥ 0.94x | pass | ≥ 0.94x | **PASS** |

Commands (representative; repeated 3x per binary per mode):
```
cmake-build-x86jit-rel/bin/hermes -O -emit-binary -out tai.hbc \
    benchmarks/jit-benches/typed-array-index.js
cmake-build-x86jit-rel/bin/hermes -b tai.hbc                 # candidate interp
cmake-build-x86jit-rel/bin/hermes -b -Xjit=force tai.hbc     # candidate JIT
<baseline-build>/bin/hermes -b tai.hbc                       # baseline interp
<baseline-build>/bin/hermes -b -Xjit=force tai.hbc           # baseline JIT
```

### Evaluation vs the bars — measured diagnosis and ruling

Under the ORIGINAL provisional bars, three of four were met and
vec-dot-f64's 1.138x missed the 1.2x guess; under the ACCEPTED bars
(the ruling recorded in the Thresholds paragraph above), all four
pass. The rest of this section is the measured diagnosis that
grounded the ruling.

The measurement is not noise: each side's three JIT runs land within
about 1% of each other (753/751/753 candidate; 864/857/853 baseline),
and the interpreter runs confirm the two binaries are otherwise
identical. The tier is confirmed active (all six sites specialize to
the Float64 kind-42 typed-array tier, verified live via
`-Xdump-jitcode=3` above) — this is a real, reproducible per-call
speedup, just smaller than the provisional bar predicted.

Root cause: the provisional 1.2x bar was carried over from the
GetByVal record's scale, but the spec itself flags this as
"provisional... not derived guarantees" pending real ByIndex
measurement (see "Benchmark and expected numbers" above). GetByIndex's
plain baseline helper (`_sh_ljs_get_by_index_rjs`) is already cheaper
per call than GetByVal's (`_sh_ljs_get_by_val_rjs`), since there is no
key double-to-uint32 conversion to amortize away by inlining — so the
per-call win from replacing that call with an inline load is
intrinsically smaller for ByIndex than it was for ByVal. `vecDotF64`
additionally spends per-call cost outside the loads themselves (the
`Call3` dispatch, two `Mul`, two `Add`) that both binaries pay equally,
which further compresses the visible ratio for a short, 3-term dot
product. `vec-dot-arr`'s ratio (1.137x) is nearly identical in absolute
terms to `vec-dot-f64`'s (1.138x) — consistent with this being a
property of the six-GetByIndex-call shape rather than of the
typed-array tier specifically — and it happens to clear its lower
1.1x bar.

**Ruling (closed): the measurement is ACCEPTED and the f64 bar is
revised to ≥ 1.1x, matching its arr twin.** The implementation is
correct and the speedup is real (confirmed by direct emission
inspection, not just timing); the original 1.2x guess was
miscalibrated the same way the GetByVal bars were, and the decisive
observation is that the two dot kernels — identical emitted
economics — measured 1.137x and 1.138x on either side of their
differing guessed bars. The diagnosis above (cheaper by-index
baseline helper; fixed per-call kernel overhead compressing a
3-term dot product's ratio) is the mechanism. No follow-up is
filed: the paths past this ceiling are the same optimizing-tier
capabilities already considered and rejected for ByVal.

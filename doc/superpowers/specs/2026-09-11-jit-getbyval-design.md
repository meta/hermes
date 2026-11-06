# JIT: inline GetByVal tiers with per-site shape feedback — design

Date: 2026-09-11. Status: draft, pre-review.
Scope: x86-64 JIT only (arm64 stays on the plain helper, dormant, as
for PutByVal). Delivers dz 01a07f0a-11f4. Companion and template:
doc/superpowers/specs/2026-09-08-jit-putbyval-typed-array-design.md and
doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md
— this design deliberately mirrors the shipped PutByVal machinery and
reuses its records, recompilation trigger, and demotion verbatim
wherever the load side does not genuinely differ.

## Motivation

GetByVal compiles to a bare call to `_sh_ljs_get_by_val_rjs` (x86-64
`Emitter::getByVal`, JitEmitter-property.cpp ~1192). The dense-array
and typed-array fast paths exist only in C++ inside the helper
(`tryFastGetComputedNoAlloc`, JSObject-inline.h:80): every read pays
the full register sync, the C call, the object dispatch, and the
f64→u32 key conversion in C++. Loads outnumber stores in most JS, and
on the wasm branch every memory read is a typed-array load whose index
was just computed by Imul — GetByVal is the read half of the pair
whose write half already got its tiers.

## Policy (settled in design discussion, 2026-09-11)

- The **JSArray load tier is emitted unconditionally, in every
  version, on every build configuration**. Loads have no write
  barrier, so unlike the PutByVal fast-array tier there is no
  `HERMES_JIT_INLINE_SAFE_STORE` gate and no per-GC or per-heap-mode
  exclusion — MallocGC included. Rationale: fast paths are stateless
  (I2 of dz 01a083c3), so a tier's hits are silent and an emitted tier
  can never be honestly removed; and recompilation cannot help a hot
  loop's first invocation (no OSR), so the ubiquitous JSArray case
  must be fast in version 1. This forecloses "emit JSArray only on
  evidence" — accepted: at pure-typed-array sites (all of wasm) the
  JSArray guard chain is dead bytes behind the TA tier's kind guard.
- The **typed-array tier is evidence-driven**, exactly as for PutByVal:
  a recompile adds an exact-kind TA tier at a site whose record holds
  a kind; the TA tier is checked first and its kind miss chains into
  the JSArray tier. Tiers are never dropped; the first observed kind
  is kept and a second kind poisons the record without replacing it.
- **No shared/stub emission.** The tiers are emitted inline per site,
  matching PutByVal structure (explicitly decided against a
  subroutine/stub form).
- **BigInt64/BigUint64 decline** and record as "other": a BigInt load
  must allocate a BigIntPrimitive, which inline code cannot do. (The
  store side has no such blocker — noted as a possible future PutByVal
  extension, out of scope here.) **Float16 declines** as for stores
  (no F16C dependency). **Uint8Clamped is SUPPORTED** — clamping is
  store-side semantics; the load is a plain uint8. Supported kinds:
  Int8/16/32, Uint8, Uint8Clamped, Uint16/32, Float32, Float64.
  Because the load set differs from the store set, kind support
  becomes TWO predicates (see Records below) — the existing
  store-only predicate cannot serve get sites.

## Records, recompilation, demotion — reused, not rebuilt

Get sites join the existing per-version `JitConsumerRecords.byValSites`
(JitFunctionData.h:96): `siteId` is the bytecode offset, so get and
put sites can never collide, and `JitByValSiteRecord`'s fields mean
the same things (`jsArraySeen` feeds diagnostics and demotion
stability only — the JSArray tier is a static prior for loads too).

- **Recording helper**: one new handler, `_jit_get_by_val(SHRuntime *,
  SHLegacyValue *source, SHLegacyValue *key, SHJitVersionData *,
  uint32_t siteId) -> SHLegacyValue`, the exact shape of
  `_jit_put_by_val_loose` (JitHandlers.cpp ~432): record via
  `recordByValObservation`, bump the shared decline counter, consider
  recompile, then tail into `_sh_ljs_get_by_val_rjs`. There is no
  strict/loose split for loads — one helper.
- **Emitted call**: through the per-site mutable `helper` slot
  (`movabs xScratch, &site.helper; call [xScratch]`), using the same
  callRuntimeIndirect machinery. The call site always passes the
  5-argument recording signature; the demoted 3-argument
  `_sh_ljs_get_by_val_rjs` ignores the extra registers (the same SysV
  compatibility the put slots rely on for 6-vs-4).
- **Demotion**: the existing pass gains the third recording-helper
  identity: `isRecordingHelper` recognizes `_jit_get_by_val`, and
  `demoteSite` flips it to `_sh_ljs_get_by_val_rjs` (the put slots
  keep their strictness-derived mapping). Same
  `kDemotionStableCrossings`, same retirement sweep, same
  `NumByValDemotions` counter (shared — a demotion is a demotion).
- **Kind predicates (the one real integration change)**: today
  `recordByValObservation` classifies a target through the store-only
  `isJitSupportedTypedArrayStoreKind` (JitFunctionData.h:30 — no
  Uint8Clamped), and BOTH the recompilation progress term and the
  demotion actionable-progress predicate re-test taKind through that
  same store predicate (JitCompiler.cpp ~264 and ~342). Left
  unchanged, a Uint8Clamped-only get site would record `otherSeen`,
  never acquire a kind, and demote tierless. The fix: add
  `isJitSupportedTypedArrayLoadKind` (the store set plus
  Uint8Clamped); `recordByValObservation` takes the site's kind
  predicate from its calling recording helper (put helpers pass the
  store predicate, the get helper passes the load predicate); the two
  progress predicates switch from the store predicate to the UNION of
  the two. The union is sound there because taKind is only ever SET
  by a recorder that validated it against its own operation's
  predicate, and each emitter's tier selection independently
  re-checks its own predicate before specializing — a put site can
  never hold (and so never "make progress toward") a load-only kind.
- **Recompilation trigger and progress**: otherwise unchanged code.
  The recording helper pools declines into the shared counter; the
  ByVal progress term asks "does some site's record hold a kind the
  body did not specialize?", which spans get sites because they live
  in the same deque. The budget stays `-Xjit-max-recompiles`
  (default 2).

## The tiers

Emission point: `getByVal` grows a `getByValImpl` mirroring
`putByValImpl` (JitEmitter-property.cpp ~386): consult the prior
version's record for the site (same mechanism as put tier selection),
emit the TA tier if a supported kind was recorded (kind miss falls
through), then the JSArray tier unconditionally, then the indirect
helper call as the shared slow path; the result lands in `frRes`
either way. `getByValWithReceiver` and `getByIndex` are untouched
(follow-ups; the receiver form stays on its helper permanently).

### JSArray fast tier (unconditional)

Semantics authority: the C++ fast path (JSObject-inline.h:90-101).
Guards and steps:

1. Source is a pointer to a cell with `CellKind::JSArrayKind`
   (exact kind, as the put tier guards; the C++ path accepts all of
   ArrayImpl — Arguments objects additionally — but the tier mirrors
   the put tier's narrower guard and lets Arguments decline).
2. Key is a number that converts EXACTLY to uint32 (reusing the put
   tier's key-conversion sequence unchanged).
3. Range: index in `[beginIndex, endIndex)` of the indexed storage
   (the inline expansion of `ArrayImpl::at`). Out of range →
   **decline to the helper**, NOT undefined: the prototype chain may
   carry indexed properties, so only the full path can answer.
4. Load the SmallHermesValue element. **Empty → decline** (a hole
   reads through to prototypes/accessors). The put tier carries the
   same empty-slot decline (JitEmitter-property.cpp ~189) — the
   read side's actual difference is elsewhere: per the C++ comment,
   no fast-index-properties flags check is needed on the read side
   at all, because non-fast index storage leaves empty slots and
   the empty check subsumes it.
5. Unbox SHV→HV into the result register. Mode-dependent but total —
   this is where loads are fundamentally easier than stores: HV64 is
   the identity; HV32/BOXED decode small ints/bools inline, add the
   heap base for compressed pointers, and a boxed double is ONE
   dereference. No case declines, no allocation exists.

### Typed-array tier (evidence-driven, exact kind)

Semantics authority: JSObject-inline.h:102-115 — note out-of-bounds
and detached typed-array reads return **undefined**, they are not
declines. Guards and steps for specialized kind K:

1. Source is a pointer cell with CellKind exactly K.
2. Key converts exactly to uint32 (same shared key sequence).
3. Bounds: index < length. Detached buffers read as length 0 /
   null data (mirror the put tier's null-`data_` handling), so
   bounds-or-detached failure branches to a tiny inline
   `mov undefined` — no helper call, no recording (nothing to learn:
   the site's shape IS the specialized kind).
4. Element load + convert to a number HV:
   - Int8/Int16: `movsx` + `cvtsi2sd`; Uint8/Uint8Clamped/Uint16:
     `movzx` + `cvtsi2sd`; Int32: `cvtsi2sd` from 32-bit;
     Uint32: zero-extend to 64-bit, `cvtsi2sd` from 64-bit.
   - Float32: `cvtss2sd`; Float64: direct load.
   - **NaN canonicalization (float kinds only, correctness pin):**
     a raw f32/f64 element can hold any NaN pattern, including ones
     that alias the NaN-box tag space; encoding it verbatim would
     forge a non-number HV. The tier must self-compare and replace
     any NaN with the canonical quiet NaN before encoding — the
     load-side mirror of the store tier's parity-decline. Integer
     kinds always produce finite doubles and skip this.
5. Encode as a number HV in the result register (trusted after
   canonicalization).

The int-kind loads produce doubles; no small-int HV encoding cleverness
in v1 (the interpreter's boxed encode does more; a number HV double is
always correct).

## Counters and flags

No new flags. `-Xjit-emit-counters` continues to report
`NumByValDemotions`; recompiles show in `NumRecompiles` as today.

## Non-goals

- GetByIndex (constant key): natural follow-up; shares the tier
  logic, deletes the key conversion. Not in v1.
- GetByValWithReceiver: stays on its helper (proxy/super traffic).
- arm64 tiers; BigInt kinds (needs inline allocation); Float16
  (needs F16C or software convert); small-int HV encoding of int
  loads; any PutByVal changes (including the MallocGC store tier and
  the BigInt store extension noted above).

## Testing

Mirrors the PutByVal suites; every policy test uses the established
recompile-test pattern (thresholds via -Xjit-recompile-threshold,
budget via -Xjit-max-recompiles, dumps via -Xdump-jitcode).

- Policy (test/jit/x86-64/): recompile-getval-{trigger (TA kind seen →
  recompile adds tier), mono-jsarray (JSArray-only site: NO recompile
  pressure beyond recording, tier present in v1), duo, poisoned both
  orders, demote, demote-delay, demote-carried, demote-retired,
  demote-budget0}.js — the get twins of the put set, with dumps
  pinning tier presence. "Duo" means what the shipped
  recompile-taval-duo.js means: ONE site seeing JSArray AND one TA
  kind — it pins the TA-kind-miss chaining INTO the retained JSArray
  tier (a distinct emitter path from poisoning), probing both
  targets' VALUES after specialization and both tiers in the dump.
  Two-TA-kind traffic is the poisoning pair, kept separate.
- Semantics (test/jit/, runs on both backends where applicable —
  arch-neutral tests exercise arm64's helper path):
  - getval-conversions.js: every supported kind round-trips known bit
    patterns through its OWN function (one CodeBlock per kind — a
    shared loader would specialize only its first kind and poison the
    rest, silently testing the helper; the shipped
    taval-conversions.js makes the same demand), warmed then
    re-invoked so the specialized body executes, over views with
    NONZERO byteOffset and nonzero indices (the addressing includes
    `offset_`; zero-offset probes cannot validate that term).
    Uint8Clamped reads; Uint32 values above 2^31.
  - NaN canonicalization pin (float kinds, in getval-conversions.js):
    write positive-payload NaNs, negative tag-space-aliasing NaNs,
    and signaling patterns through an aliased integer view, read
    through the specialized f32 and f64 load tiers, and require
    `x !== x` AND `Object.is(x, NaN) === true` for each. The
    Object.is oracle is the load-bearing one: `isSameValue`
    recognizes only the canonical NaN payload (sign bit masked)
    before falling back to raw-bit equality, so a load that RETAINS
    a noncanonical payload fails it, while `typeof` and plain NaN
    arithmetic would pass. Diffed against the interpreter.
  - getval-guards.js: holes decline — a sparse JSArray over a
    prototype carrying indexed data properties AND an indexed
    accessor must return the prototype's answers under the JIT;
    out-of-range JSArray reads find prototype properties; TA
    out-of-bounds reads are undefined (also for huge and boundary
    indices); detached TA reads are undefined (detach helper per the
    put-side tests' mechanism); non-uint32 keys (negative,
    fractional, ≥2^32, string, -0) match the interpreter; Arguments
    objects decline to the helper and still answer correctly.
  - getbyval-inline-emitted.js (+ nothing on arm64 — no tier): pins
    the indirect recording call (`mov r11, {{.*}}` +
    `call qword ptr [r11]`), the JSArray tier's empty-check decline,
    and a specialized TA tier's load+convert instruction.
- Interpreter-vs-JIT diff test over a mixed workload (the
  putbyval-inline.js pattern), compiled -fstatic-builtins where
  Math.* is involved or plain otherwise.
- Heap modes: full jit suite green on HV64 ASan, HV32, BOXED. NEW
  gate vs the put feature: the jit suite also RUNS on the MallocGC
  tree (cmake-build-x86jit-malloc) — the load tier is live there,
  unlike the store tier, so MallocGC graduates from build-only to
  build+jit-suite.
- Prove-can-fail: mutate the empty-check to fall through (named
  guard test fails); mutate NaN canonicalization away (conversion
  test fails); mutate the TA bounds check to <= (OOB test fails);
  restore, REBUILD, re-run green — rebuild after restore is
  mandatory (stale-binary incident, twice).

## Benchmarks and expected numbers

benchmarks/jit-benches/typed-array-load.js, committed, mirroring
typed-array-store.js: ta-load (Int32Array sum loop), arr-load (dense
JSArray sum loop), f16-load (permanently-declining control),
poisoned-load (Int32/Float64 both orders), plus an arr-holes control
(sparse array — measures the decline path's recording tax and its
demotion recovery). Method per benchmarks convention: 1000x20000,
Release, precompiled with -emit-binary, interleaved A/B vs the
pre-feature base, medians of 3.

Thresholds — REVISED at delivery (user ruling, 2026-09-11). The
original bars (ta-load ≥ 2.5x, poisoned ≥ 1.3x, controls ≥ 0.97x)
were calibrated by analogy to the PutByVal feature and were wrong
for load economics, as the measured diagnosis showed: the baseline
GET helper is 2.17x cheaper than the put helper for the same call
count, so there was less to remove (pure-read ceiling of the current
tier shape: 1.81x); a 50/50 poisoned mix has a 1.25x Amdahl ceiling
with a single-kind tier; and pointer-flip demotion has an intrinsic
+2 cycle/call floor (failed guard chain + indirect call) on
permanently-declining sites. Accepted thresholds against the same
A/B method: ta-load ≥ 1.5x; arr-load ≥ 1.15x; poisoned ≥ 1.1x
either order; f16-load and arr-holes ≥ 0.93x after demotion
settles (restated from a rounded 0.94x after external review noticed
the unrounded f16 ratio is 0.9362 -- the ruling accepted the MEASURED
values, so the floor follows the measurement, not its rounding). The practical paths past the original bars are filed as
follow-ups (demote-by-code-patching; two-kind TA tier — see the dz
issues cited in Delivered) and were deliberately not folded into
this feature; shortening the tier itself past its measured 1.81x
pure-read ceiling was judged to require optimizing-tier capabilities
(loop-invariant reasoning, emitter-wide value tracking) this JIT
does not have, and is deliberately NOT tracked as an issue.
Delivered numbers recorded below.

## Delivered (Task 5, measured 2026-09-11)

Candidate: `cmake-build-x86jit-rel` rebuilt at tip 992b5c6a6 (Release,
clang/clang++, HERMESVM_ALLOW_JIT=2, HERMESVM_GCKIND=HADES,
HEAP_HV_64, no sanitizers). Baseline: a throwaway `git worktree` at the
pre-feature tip 435d56938, configured with the identical toolchain and
options (verified key-by-key against the candidate's CMakeCache) and
built the same way; removed after measurement. Both interpreter and
`-Xjit=force` runs share ONE `.hbc` artifact, compiled once with the
candidate's `hermes -O -emit-binary` and confirmed byte-identical
(`cmp`, exit 0, 4076 bytes both sides) to what the baseline compiler
produces for the same source — bytecode is unaffected by this feature,
so this is the expected result, not a coincidence.

Commands:
```
hermes -O -emit-binary -out tal.hbc benchmarks/jit-benches/typed-array-load.js
hermes -b tal.hbc              # interpreter, x3 each binary
hermes -b -Xjit=force tal.hbc  # JIT, x3 each binary
```

All runs, ms (three per cell, machine otherwise idle, runs sequential,
one extra spot-check pair confirmed stability beyond noise):

| bench | cand interp | cand JIT | base interp | base JIT |
|---|---|---|---|---|
| ta-load | 135, 138, 133 | 72, 71, 71 | 157, 137, 134 | 116, 119, 116 |
| arr-load | 114, 112, 114 | 72, 70, 71 | 111, 113, 114 | 107, 108, 107 |
| f16-load | 156, 151, 152 | 141, 142, 140 | 150, 154, 155 | 135, 132, 131 |
| poisoned-load | 133, 132, 130 | 102, 103, 103 | 130, 131, 130 | 118, 116, 121 |
| poisoned-load-rev | 131, 134, 135 | 99, 99, 102 | 132, 133, 133 | 116, 115, 117 |
| arr-holes | 709, 712, 711 | 730, 726, 727 | 708, 709, 716 | 695, 693, 705 |

Medians and ratios (base-JIT median / cand-JIT median; >1 favors the
candidate):

The table shows both threshold generations: the ORIGINAL bars (which
five rows missed, triggering the diagnosis) and the ACCEPTED bars
from the ruling (see the revised Thresholds paragraph above), which
ALL SIX rows meet.

| bench | cand JIT | base JIT | ratio | original | met? | accepted | met? |
|---|---|---|---|---|---|---|---|
| ta-load | 71 | 116 | 1.63x | ≥ 2.5x | no | ≥ 1.5x | **yes** |
| arr-load | 71 | 107 | 1.51x | ≥ 1.15x | yes | ≥ 1.15x | **yes** |
| f16-load | 141 | 132 | 0.9362x | ≥ 0.97x | no | ≥ 0.93x | **yes** |
| poisoned-load | 103 | 118 | 1.15x | ≥ 1.3x | no | ≥ 1.1x | **yes** |
| poisoned-load-rev | 99 | 116 | 1.17x | ≥ 1.3x | no | ≥ 1.1x | **yes** |
| arr-holes | 727 | 695 | 0.956x | ≥ 0.97x | no | ≥ 0.93x | **yes** |

Under the original bars only arr-load cleared comfortably; the
five misses are what triggered the measured diagnosis, whose ceiling
arguments produced the accepted bars — under which the feature
passes on every row. A repeat pair of
JIT-mode runs (candidate 71/139/104/101/726, baseline 118/136/116/116/692
for ta-load/f16-load/poisoned-load/poisoned-load-rev/arr-holes)
reproduced the same magnitudes, so this is not run-to-run noise.

Correctness (`getval-conversions.js`, `getval-guards.js`, the recompile
policy suite, the demotion suite) all pass — the shortfall is
performance only, not a functional defect. Plausible mechanism,
offered as a hypothesis, not verified by instrumentation: GetByVal's
pre-feature helper path was already a comparatively cheap C++ fast
path (`tryFastGetComputedNoAlloc`) reached from both the interpreter
and the old JIT call, so removing the call/dispatch overhead recovers
less than PutByVal's equivalent removal did (PutByVal's pre-feature
path carried more machinery -- property-cache maintenance, barrier
consideration -- so inlining it had more to remove); and for the
control benchmarks (f16-load, poisoned's declining half, arr-holes),
the post-demotion path still calls through the per-site indirect
`helper` slot (`movabs`+`call [slot]`, one load and one indirect branch
more than the baseline's direct call), a fixed per-call tax that a
tight 20-million-iteration loop cannot amortize away. Neither
explanation was chased into the codegen or reverted; per this task's
brief, a missed threshold is reported, not massaged.

### Ruling and follow-ups (2026-09-11)

The user accepted the measured numbers; the Thresholds paragraph
above was revised accordingly. The full measured diagnosis is
doc/superpowers/specs/2026-09-11-jit-getbyval-perf-diagnosis.md.
The diagnosis (helper-cost
asymmetry 2.17x, pure-read ceiling 1.81x for the current tier shape,
+2 cycle/call demotion floor, 1.25x Amdahl ceiling on 50/50 poisoned
traffic) confirmed the original bars were put-side analogies, not
load-side realities. Shortening the tier past its 1.81x pure-read
ceiling (guard hoisting, integral-index tracking, unbox fusion) was
considered and rejected as a tracked follow-up: each lever is an
optimizing-tier capability, not an increment on this JIT. The
practical paths are filed as:

- dz 01a0900d-22e7 — demote-by-code-patching (removes the fixed
  per-call tax on permanently-declining sites)
- dz 01a0900d-22ff — two-kind typed-array tier for poisoned sites
  (est. 1.69x on the 50/50 mix)
- dz 01a0900d-2318 — the pre-existing MallocGC ById-timing
  divergence discovered by the new MallocGC jit-suite gate
  (9 tests to un-gate once fixed)

# JIT GetByVal Inline Tiers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inline JSArray and evidence-driven typed-array load tiers for
GetByVal on the x86-64 JIT, with per-site shape feedback, recompilation,
and demotion reusing the shipped PutByVal machinery.

**Architecture:** Get sites join the existing JitByValSiteRecord deque;
one new recording helper; a two-predicate kind-support split (load set
adds Uint8Clamped); getByValImpl mirrors putByValImpl (TA tier first
when specialized, JSArray tier always, indirect helper call through the
per-site mutable slot).

**Tech Stack:** C++17, asmjit x86-64, lit/FileCheck.

**Spec:** doc/superpowers/specs/2026-09-11-jit-getbyval-design.md
(Codex-reviewed, all findings CLOSED; binding). Its companions
2026-09-08 and 2026-09-09 putbyval specs describe the machinery being
reused.

## Global Constraints

- The JSArray load tier is emitted UNCONDITIONALLY in every version on
  every build config (no barrier on loads → no Config.h gate, MallocGC
  included). The TA tier is evidence-driven, exact-kind, first kind
  kept, second kind poisons. Tiers are never dropped. No fast-path
  instrumentation.
- Load-supported kinds: Int8/16/32, Uint8, Uint8Clamped, Uint16/32,
  Float32, Float64. Float16 and BigInt kinds decline and record via the
  site's predicate as "other".
- Semantics authority is JSObject-inline.h:20-118: JSArray hole OR
  out-of-range → DECLINE to helper (prototype chain); TA out-of-bounds
  or detached → inline `undefined`, no helper, no recording; float
  loads canonicalize ANY NaN pattern before HV encoding.
- Kind predicates: recordByValObservation classifies through a
  predicate supplied by its calling recording helper (put → existing
  store predicate; get → new isJitSupportedTypedArrayLoadKind = store
  set + Uint8Clamped). The recompile progress term (JitCompiler.cpp
  ~264) and demotion actionable predicate (~342) switch to the UNION
  of the two predicates.
- The recording helper is `_jit_get_by_val(SHRuntime *, SHLegacyValue
  *source, SHLegacyValue *key, SHJitVersionData *, uint32_t siteId)`
  returning SHLegacyValue; sites call through the per-site helper slot
  (movabs + indirect call), always passing 5 args; demotion flips the
  slot to the 3-arg `_sh_ljs_get_by_val_rjs` (SysV extra-arg
  compatibility, same as put's 6-vs-4). The indirect call MUST use
  `callRuntimeWithSavedIPIndirect` — the fallback can run getters and
  throw, and today's getByVal already saves the IP via
  EMIT_RUNTIME_CALL; the unsaved variant would break that contract.
- STAGING RULE: recording activates only WITH the TA tier (Task 3).
  Until then get sites emit the plain direct helper call — otherwise
  the intermediate tree records actionable TA kinds that no emitter
  can specialize, qualifying useless recompiles until budgets expire.
- EVERY new policy test drives its hot site with a PARAMETERIZED
  (dynamic) key and pins that the site is GetByVal: constant uint8
  keys lower to GetByIndex (ISel.cpp ~1271), which this feature does
  not touch — a constant-key "twin" of a put test silently tests
  nothing.
- No BytecodeVersion.h changes; no bytecode changes at all. arm64
  emitter untouched (its getByVal stays the bare helper call).
- Runtime C++ (lib/VM/) implementers invoke `gc-safe-coding` first.
- 80 cols, 2-space indent. Builds: x86 ASan
  `cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-x86jit --target hermes`;
  x86 jit suite `(cd /home/tmikov/work/hermes-x86-jit && LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes)`;
  full suite = no filter; arm64/HV32/BOXED/malloc trees analogous
  (rebuild cmake-build-host FIRST if anything renumbers — nothing here
  does — and always rebuild a tree before its suite). Never bare `cd`.
- Prove-can-fail: after every mutation, restore AND REBUILD before the
  green re-run (stale-binary incident, twice).
- Benchmarks: precompiled with -emit-binary, Release tree, interleaved
  A/B, medians of 3, machine otherwise idle.
- The spec and this plan stay UNCOMMITTED (folded at squash time, per
  branch convention). Code+test commits per task as specified.
- Commits end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB`.

## File Structure

Task 1 (records/handlers): include/hermes/VM/JIT/JitFunctionData.h
(load predicate), lib/VM/JIT/JitHandlers.cpp (+ its header decl,
recordByValObservation predicate param, _jit_get_by_val),
lib/VM/JIT/JitCompiler.cpp (progress predicates → union; demotion
recognizes the get recording helper and flips to
_sh_ljs_get_by_val_rjs).
Task 2 (JSArray tier, direct helper call): lib/VM/JIT/x86-64/
JitEmitter-property.cpp (getByValImpl, emitGetByValFastArrayTier),
JitEmitter.h decls; tests test/jit/getval-guards.js (arch-neutral),
test/jit/x86-64/getbyval-inline-emitted.js (pins only).
Task 3 (recording + TA tier + trigger): JitEmitter-property.cpp
(indirect recording call via site slot, emitGetByValTypedArrayTier,
tier selection from prior records); tests test/jit/
getval-conversions.js and getbyval-inline.js (mixed diff workload,
both arch-neutral), recompile-getval-{trigger,duo,
poisoned-int32-first,poisoned-float64-first}.js (x86-64); guards
test gains TA OOB/detached sections; emitted test gains the
indirect-call and TA pins.
Task 4 (demotion + mono policy tests): recompile-getval-{
mono-jsarray,demote,demote-delay,demote-carried,demote-retired,
demote-budget0}.js (x86-64).
Task 5 (validation/bench/docs): benchmarks/jit-benches/
typed-array-load.js, doc/JIT.md, spec Delivered section.

---

### Task 1: Records, predicates, recording helper, demotion identity

- [ ] **Step 1:** In JitFunctionData.h beside
`isJitSupportedTypedArrayStoreKind` (~line 30) add
`isJitSupportedTypedArrayLoadKind`: the store set plus
`CellKind::Uint8ClampedArrayKind`. Doc-comment states the load/store
asymmetry (clamping is store-side; BigInt loads allocate).
- [ ] **Step 2:** `recordByValObservation` (JitHandlers.cpp ~392)
gains a kind-predicate parameter; the two put recording helpers pass
the store predicate; add `_jit_get_by_val` per the Global Constraints
signature: record with the load predicate, bump the shared decline
counter, considerRecompile at threshold, then tail into
`_sh_ljs_get_by_val_rjs` RETURNING its value. Declare beside the put
helpers' declarations (match their header placement).
- [ ] **Step 3:** JitCompiler.cpp: the progress term (~264) and the
demotion actionable predicate (~342) switch from the store predicate
to the union (a small `isJitSupportedTypedArrayKindForSomeByValOp`
helper or explicit ||, matching local style). `isRecordingHelper`
(demotion pass) recognizes `_jit_get_by_val`; `demoteSite` maps it to
`_sh_ljs_get_by_val_rjs` (put slots keep their strictness mapping).
- [ ] **Step 4:** Nothing installs the new helper yet (the emitter
still emits the bare direct call), so behavior is unchanged: build
x86 ASan; run the FULL x86 suite AND the x86 jit suite — green
unchanged is the gate. The existing recompile-taval-* tests are the
regression net for the predicate refactor.
- [ ] **Step 5:** Commit — `JIT: ByVal record plumbing for load sites`

### Task 2: JSArray load tier (slow path stays the direct helper)

- [ ] **Step 1:** Split `getByVal` (JitEmitter-property.cpp ~1192)
into `getByValImpl` mirroring `putByValImpl`'s (~386) structure, but
in THIS task the slow path remains today's DIRECT
`EMIT_RUNTIME_CALL(_sh_ljs_get_by_val_rjs)` — no site records, no
recording (Global Constraints staging rule). The tier's guard
failures branch to that slow-path call; result into frRes on both
paths.
- [ ] **Step 2:** `emitGetByValFastArrayTier`, modeled instruction-
for-instruction on `emitPutByValFastArrayTier` (~26) MINUS the
store/barrier machinery: exact JSArrayKind guard, the put tier's key
uint32-exact conversion reused verbatim, [beginIndex, endIndex)
range check (out of range → slow label), SHV element load, empty →
slow label, SHV→HV unbox into the result register (HV64 identity;
HV32/BOXED: inline small decode, compressed-pointer add, boxed-double
ONE deref — total, never declines). Emitted in EVERY version,
unconditionally, before the slow path; when Task 3 later adds a TA
tier it goes in FRONT (kind-miss chains here).
- [ ] **Step 3:** Tests, all green together with the suites; every
test drives sites with DYNAMIC keys (Global Constraints — constant
keys lower to GetByIndex):
  - test/jit/getval-guards.js (ARCH-NEUTRAL — arm64 runs it through
    its helper path; interpreter-diffed like taval-guards.js): holes
    over a prototype with BOTH an indexed data property and an
    indexed accessor; out-of-range reads finding prototype
    properties; non-uint32 keys (negative, fractional, 2^32, string,
    -0) matching the interpreter; Arguments objects (decline,
    correct values); a frozen/sealed dense array reads correctly.
  - test/jit/x86-64/getbyval-inline-emitted.js: pins the JSArray
    kind guard, the empty-check branch, and that the hot function's
    bytecode contains GetByVal (anti-GetByIndex anti-vacuity pin).
- [ ] **Step 4:** Prove-can-fail: mutate the empty check to fall
through — the guards test's prototype-hole section FAILS by name;
mutate the range check upper bound to a constant — guards test
fails. Restore, REBUILD, re-run green.
- [ ] **Step 5:** Full x86 suite + x86 jit suite green; commit —
`JIT: inline GetByVal fast-array tier`

### Task 3: Recording + typed-array load tier + recompile trigger

- [ ] **Step 0:** Switch getByValImpl's slow path to the INDIRECT
call through `&site.helper` initialized to `_jit_get_by_val`:
find/create the site record (bytecode offset as siteId, same
prior-version record consultation as put), 5 args,
`callRuntimeWithSavedIPIndirect` (MANDATORY — Global Constraints),
result from rax into frRes, recordingByValSites accounting as for
put sites. Recording and the TA tier land TOGETHER in this task so
every recorded kind is actionable by the same tree's emitter.
- [ ] **Step 1:** `emitGetByValTypedArrayTier`, modeled on
`emitPutByValTypedArrayTier` (~201): exact-kind guard for the
specialized kind, shared key conversion, null-data/detached and
bounds check where FAILURE branches to a local `mov undefined` into
the result (inline, no helper, no recording — spec rule), per-kind
element load + convert (movsx/movzx + cvtsi2sd; Int32 cvtsi2sd
32-bit; Uint32 zero-extend + 64-bit cvtsi2sd; Float32 cvtss2sd;
Float64 movsd), and for the two float kinds the NaN
canonicalization: self-compare, on unordered load the canonical
quiet-NaN constant, THEN encode as a number HV.
- [ ] **Step 2:** Tier selection in getByValImpl: prior-version
record holds a load-supported kind → emit the TA tier FIRST, kind
miss chains into the JSArray tier; record `specializedTAKind`.
Mirrors putByValImpl ~437-467 including targetKnownObject handling.
- [ ] **Step 3:** Tests (dynamic keys everywhere; each policy test
pins GetByVal presence in its hot function's bytecode or dump):
  - test/jit/getval-conversions.js (ARCH-NEUTRAL): ONE FUNCTION PER
    KIND
    (shared-loader poisoning hazard — spec mandate), warmed then
    re-invoked so the specialized body runs, views with NONZERO
    byteOffset and nonzero indices; known bit patterns per kind;
    Uint8Clamped reads; Uint32 > 2^31; float NaN families
    (positive-payload, negative tag-aliasing, signaling — via an
    aliased integer view) with `x !== x` AND `Object.is(x, NaN)`
    oracles; diffed against the interpreter.
  - recompile-getval-trigger.js: TA kind at a get site → threshold
    crossing → recompile adds the tier (dump pins version 2 + the
    tier); values correct before and after.
  - recompile-getval-duo.js: ONE site seeing JSArray AND Int32Array —
    after specialization the dump pins BOTH tiers and both targets
    read correct values (TA-kind-miss chaining pin).
  - recompile-getval-poisoned-int32-first.js and
    -float64-first.js: two TA kinds in either order — first kind's
    tier only, second kind's traffic correct via helper.
  - getval-guards.js gains: TA out-of-bounds → undefined (boundary,
    huge, and just-past-end indices), detached → undefined (detach
    mechanism as in the put-side tests), Float16 and BigInt64 arrays
    decline and answer correctly.
  - test/jit/getbyval-inline.js (ARCH-NEUTRAL): the spec's mixed
    interpreter-vs-JIT differential workload over arrays, several TA
    kinds, holes, and OOB, modeled on putbyval-inline.js — both
    backends run it (arm64 exercises its helper path).
  - getbyval-inline-emitted.js gains the indirect recording-call pin
    (`mov r11, {{.*}}` + `call qword ptr [r11]`) and a
    specialized-site section pinning the kind guard, one integer
    convert, the float canonicalization compare, and the bounds-fail
    undefined path.
- [ ] **Step 4:** Prove-can-fail: remove the NaN canonicalization —
the Object.is oracle FAILS by name; change the bounds check to <= —
the just-past-end undefined case FAILS; restore, REBUILD, green.
- [ ] **Step 5:** Full x86 suite + x86 jit suite green; commit —
`JIT: evidence-driven GetByVal typed-array tier`

### Task 4: Demotion and mono policy for get sites

- [ ] **Step 1:** The get twins of the put demotion tests — modeled
on recompile-taval-demote{,-delay,-carried,-retired,-budget0}.js for
their FLAG RECIPES and dump/counter techniques, but NOT file-for-file
(a constant-key READ like `obj[0]` lowers to GetByIndex — fine in
the put twins, whose stores stay PutByVal, but fatal in a read
translation): every hot site takes a parameterized key, and every
test pins GetByVal presence. Coverage: stable-crossings demotion of
a get site (NumByValDemotions counter), delayed demotion while the
record still changes, demotion state carried across recompiles,
retirement sweep flipping still-recording get slots, and budget-0
demoting without ever recompiling. Use -Xjit-recompile-threshold and
-Xjit-max-recompiles as the put twins do.
- [ ] **Step 1b:** recompile-getval-mono-jsarray.js, with the
CORRECTED recipe (the put twin triggers a ById-driven recompile and
cannot be copied; dense hits generate no crossings; the dump never
prints "(version 1)"): drive threshold crossings via repeated
HOLE/out-of-range reads through one dynamic-key GetByVal site with
no other decline sources in the function; pin the UNSUFFIXED initial
compilation line and the ABSENCE of any "(version" dump; pin the
site's demotion via the NumByValDemotions counter; values stay
correct throughout (holes resolve through the prototype).
- [ ] **Step 2:** Prove-can-fail on one: disable the retirement
sweep (scratch mutation) — demote-retired FAILS; restore, REBUILD,
green.
- [ ] **Step 3:** x86 jit suite green; commit —
`JIT: GetByVal demotion policy tests`

### Task 5: Cross-mode validation, benchmark, docs

- [ ] **Step 1:** Rebuild + jit suite green on: cmake-build-x86jit-hv32,
-boxed, AND cmake-build-x86jit-malloc (NEW gate — the load tier is
live under MallocGC, unlike the store tier). The malloc tree's lit
target already exists (its config passes jit_enabled/jit_arch and
gc=MALLOC), but the suite has PRE-EXISTING expected failures there:
putbyval-inline-emitted.js and putbyid-inline-emitted.js pin the
barrier-gated store tiers, which Config.h compiles out under
MallocGC. Gate those tests with the EXISTING `gc_malloc` lit feature
(test/lit.cfg:97 — do not invent a new one): mark the store-tier pin
tests UNSUPPORTED under it — the new load tests stay enabled
everywhere. List every gated test in
the report. arm64: build + jit suite green (dormant — no emission
change; sanity only).
- [ ] **Step 2:** benchmarks/jit-benches/typed-array-load.js,
committed, mirroring typed-array-store.js's structure and header
recipe: ta-load (Int32Array sum), arr-load (dense array sum),
f16-load (declining control), poisoned-load both orders,
arr-holes (sparse-array control measuring the decline/demotion
path). 1000x20000 shapes. A/B vs the pre-feature tip: candidate =
cmake-build-x86jit-rel (rebuilt); baseline = throwaway worktree at
the pre-feature tip with a Release build configured with THE SAME
native toolchain and options as the candidate tree (clang/clang++,
matching flags — read the candidate's CMakeCache and mirror it).
Bytecode is unchanged by this feature, so compile the benchmark
ONCE with the candidate compiler, verify the baseline compiler
produces a byte-identical .hbc (cmp), and run that one shared
artifact on both binaries; if the artifacts differ, stop and
investigate before measuring.
Thresholds (spec): ta-load ≥ 2.5x under -Xjit=force; arr-load ≥
1.15x; f16-load and arr-holes ≥ 0.97x after demotion settles;
poisoned ≥ 1.3x either order. Record ALL numbers.
- [ ] **Step 3:** doc/JIT.md: extend the ByVal shape-feedback /
recompilation section with the get tiers (JSArray unconditional +
evidence TA + shared records/demotion, the load-kind predicate
split, the TA undefined-inline rule); spec gains its Delivered
section with the measured numbers. The spec+plan files themselves
stay uncommitted.
- [ ] **Step 4:** Full x86 suite once more; commit —
`JIT: GetByVal tiers delivered (docs + benchmark)` (benchmark may
also ride the Task 5 commit — one commit for this task).

## Self-Review Notes (plan time)

- Spec coverage: predicates/records/helper/demotion identity (T1),
  unconditional JSArray tier + hole/proto semantics (T2), recording
  call + TA tier + NaN pin + trigger/duo/poison (T3), demotion +
  mono policy (T4), MallocGC-now-runs-jit-suite + benchmark thresholds + docs
  (T5). Non-goals honored (no GetByIndex/WithReceiver, no arm64, no
  BigInt/F16 tiers, no stub form, no put-side changes).
- Behavioral steps are sequenced test-with-change: T1 is inert
  (nothing installs the helper), T2 lands the tier with the direct
  helper (no recording — the staging rule keeps TA evidence
  unactionable states out of the tree), T3 lands recording + TA tier
  + trigger atomically, each with its tests and suite gates.
- Both spec-mandated inline-undefined and decline rules appear in
  Global Constraints AND the owning tasks.
- Delegated with instructions: exact header placement of the helper
  decl, the malloc tree's lit wiring, detach mechanism — each
  "match the put-side precedent and say which".

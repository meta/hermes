# JIT GetByIndex Inline Tiers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inline JSArray and evidence-driven typed-array tiers for the
constant-key GetByIndex opcode on the x86-64 JIT, sharing the ByVal
records/recompile/demotion machinery and a newly factored per-kind
element-load tail.

**Architecture:** Extract the typed-array element access tail (load +
convert + NaN canonicalization + encode, 9 kinds) from
emitGetByValTypedArrayTier into one shared helper, proven
byte-identical; then GetByIndex gets the GetByVal tier structure with
the key block deleted (K is an emit-time uint8 constant) and
begin-relative JSArray addressing kept.

**Tech Stack:** C++17, asmjit x86-64, lit/FileCheck.

**Spec:** doc/superpowers/specs/2026-09-12-jit-getbyindex-design.md
(Codex-reviewed, 4 rounds, all findings CLOSED; binding). Its binding
companion is the shipped 2026-09-11-jit-getbyval-design.md.

## Global Constraints

- Policy identical to GetByVal by reference: JSArray tier
  unconditional every version/config; TA tier evidence-driven exact
  kind; monotone; no stub emission; load-kind predicate used AS IS.
- ANTI-VACUITY (inverted from ByVal): every test site needs a LITERAL
  uint8 key (0-255) and pins `GetByIndex` in the bytecode dump — a
  non-literal or ≥256 key lowers to GetByVal and tests the wrong
  opcode.
- JSArray addressing is BEGIN-RELATIVE even with constant K, and the
  executable x86 form is (asmjit has no sub(Imm, Gp)):
  `mov idx32, K` then `sub idx32, dword ptr [beginIndex_]`, then
  `cmp idx32, dword ptr [elemCount_]` + `jae helper` — the uint32
  wrap makes the ONE unsigned compare cover both K < begin and
  K ≥ end, and the 32-bit ops zero-extend for the scaled addressing.
  Failure → helper (prototype may answer). A constant K does NOT
  make the storage offset constant.
- TA tier: exact-kind guard; bounds with the operand order and
  condition MATCHED: `cmp dword ptr [length], Imm(K)` pairs with
  `jbe undefined` (the shipped ByVal form is the reversed-operand
  `cmp idx, [length]` + `jae` — pick one order and its correct
  condition, strict acceptance either way); bounds/detached failure
  → inline undefined, no helper, no recording; element access at
  constant displacement THROUGH THE SHARED TAIL.
- Recording helper `_jit_get_by_index(SHRuntime *, SHLegacyValue
  *source, uint32_t key, SHJitVersionData *, uint32_t siteId) ->
  SHLegacyValue` (key BY VALUE, in edx, matching the plain helper);
  emitted via the per-site slot with `callRuntimeWithSavedIPIndirect`
  (5 args; demoted 3-arg plain helper ignores rcx/r8d).
- The site helper slot is initialized ONLY WHEN NULL (getByValImpl
  ~1614 is the model) — unconditional installation would silently
  restart recording after any recompile; the carried-demotion test
  pins this.
- Task 1's byte-identity gate: compare FINALIZED executable bytes of
  a representative ByVal workload pre/post refactor via a LOCAL
  UNCOMMITTED post-relocation hook (dump or hash per compiled body),
  normalizing only identified address relocations. Neither the
  -Xdump-jitcode text logger nor asmjit kMachineCode emission
  logging qualifies (offsets masked during emission; relocation
  happens later). Text dump = supplementary only.
- No BytecodeVersion.h changes. No DefineOwnByIndex changes. arm64
  untouched.
- Runtime C++ (lib/VM/) implementers invoke `gc-safe-coding` first.
- Prove-can-fail: restore AND REBUILD before every green re-run.
- Builds/suites: as the GetByVal plan (cmake-build-x86jit ASan HV64
  primary; -hv32, -boxed, -malloc, cmake-build-arm64 with
  cmake-build-host rebuilt first, cmake-build-hs-hv32 handle_san;
  jit suite = LIT_FILTER='jit/'). Never bare `cd`; foreground
  commands with generous timeouts.
- Spec and this plan stay UNCOMMITTED (fold at squash). Commits end
  with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB`.

## File Structure

Task 1 (factoring): lib/VM/JIT/x86-64/JitEmitter-property.cpp (extract
tail; ByVal tier calls it), JitEmitter.h decl if the helper is a
member; scratch-only finalization hook (NOT committed).
Task 2 (records/handler): lib/VM/JIT/JitHandlers.cpp (+ header decl),
lib/VM/JIT/JitCompiler.cpp (4th helper identity in
isRecordingHelper/demoteSite).
Task 3 (JSArray tier, direct slow path): JitEmitter-property.cpp
(getByIndexImpl + emitGetByIndexFastArrayTier), JitEmitter.h; tests
test/jit/getidx-guards.js (JSArray + storage-state sections),
test/jit/x86-64/getbyindex-inline-emitted.js (JSArray pins +
anti-vacuity pin).
Task 4 (recording + TA tier): JitEmitter-property.cpp (indirect
recording slow path, TA tier via the shared tail, tier selection);
tests test/jit/getidx-conversions.js; guards TA/boundary sections;
emitted file gains indirect-call pin + ALL-NINE-KIND version-2 pins;
policy recompile-getidx-{trigger,duo,demote,demote-carried,
budget0}.js (one poisoned order folded into trigger or standalone —
implementer states which).
Task 5 (validation/bench/docs): benchmarks/jit-benches/
typed-array-index.js, doc/JIT.md, spec Delivered.

---

### Task 1: Factor the shared typed-array element-load tail

- [ ] **Step 1:** Extract the per-kind element access tail of
`emitGetByValTypedArrayTier` (~JitEmitter-property.cpp:1400-1483:
the per-kind switch building sized/scaled memory operands, converts,
float NaN canonicalization, number-HV encode, and the branch to the
caller's doneLab) into one helper (working name
`emitTypedArrayElementLoad`) parameterized by kind, the address
basis (base register + optional index register + displacement — the
asmjit Mem operand), the result register, BOTH caller-selected
scratch registers the switch consumes (temp1 and valTmp — the
helper allocates NOTHING; all allocator bookkeeping stays in the
caller, completed before emission as today ~1549), and the caller's
labels. The seam: the helper begins AFTER the caller's
`a.add(loc, xScratch)` address setup and ends BEFORE
`a.bind(undefLab)` — the undefined block and the local doneLab
binding remain in the tier caller (the tail's branches skip that
block; the outer caller jumps to contLab, ~1478/~1595). The ByVal
tier passes its current register-indexed operand shape; NO extra
address materialization; preserve instruction order, operand sizes,
and label layout exactly.
- [ ] **Step 2:** Byte-identity gate per Global Constraints: add the
LOCAL post-relocation hook (scratch patch, never committed), capture
per-body finalized bytes for a representative ByVal workload (e.g.
test/jit/getval-conversions.js + typed-array-load.js compiled with
-Xjit=force) at the pre-refactor revision and at the refactored
tree; diff modulo identified embedded addresses — any other
difference fails the task. Keep the -Xdump-jitcode text diff as
supplementary evidence. REMOVE the hook (verify git status clean).
- [ ] **Step 3:** Full x86 suite + x86 jit suite green (unchanged
counts). Commit — `JIT: factor the typed-array element-load tail`
- [ ] **Step 4:** Prove-can-fail deferred to Task 4 (the shared-tail
NaN mutation must fail BOTH features' tests — only meaningful once
ByIndex consumes the tail).

### Task 2: Recording helper + demotion identity (inert)

- [ ] **Step 1:** `_jit_get_by_index` per Global Constraints, beside
`_jit_get_by_val` (same record-with-load-predicate → decline count →
considerRecompile → return plain-helper-result order); declaration
beside its sibling's.
- [ ] **Step 2:** `isRecordingHelper` gains the fourth identity;
`demoteSite` maps it to `_sh_ljs_get_by_index_rjs`; existing
mappings untouched.
- [ ] **Step 3:** Inert gate: build; full x86 suite + jit suite green
UNCHANGED (nothing installs the helper). Commit —
`JIT: ByIndex record plumbing`

### Task 3: JSArray tier (slow path stays the direct helper)

- [ ] **Step 1:** Split `getByIndex` into `getByIndexImpl`: emit
`emitGetByIndexFastArrayTier` (object + exact-JSArrayKind guard;
begin-relative constant-K sequence per Global Constraints; empty →
slow; shared Emit_sh_shv_decode unbox), slow path = today's DIRECT
`EMIT_RUNTIME_CALL(_sh_ljs_get_by_index_rjs)`. No records yet
(staging rule: recording lands with the TA tier).
- [ ] **Step 2:** Tests:
  - test/jit/getidx-guards.js (ARCH-NEUTRAL, interpreter-diffed;
    LITERAL keys, GetByIndex bytecode pin): holes at constant index
    over a prototype with indexed data AND accessor props; constant
    index beyond a short array finding prototype props; NONZERO
    beginIndex_ hit (first write at a high index) read at K in its
    live range; beginIndex_ > K (decline → prototype answers);
    empty/storage-less array; frozen/sealed dense arrays.
  - test/jit/x86-64/getbyindex-inline-emitted.js: JSArray kind guard
    + the immediate begin-relative compare + empty-check branch +
    the anti-vacuity GetByIndex pin.
- [ ] **Step 3:** Prove-can-fail: disable the empty check — the
prototype-hole case fails by name; break the begin-relative compare
(e.g. compare K directly vs elemCount_) — the nonzero-beginIndex
case fails. Restore, REBUILD, green.
- [ ] **Step 4:** Full x86 suite + x86 jit + arm64 jit (arch-neutral
test via helper path; host tree first) green. Commit —
`JIT: inline GetByIndex fast-array tier`

### Task 4: Recording + typed-array tier

- [ ] **Step 1:** Switch getByIndexImpl's slow path to the INDIRECT
recording call per Global Constraints (slot init ONLY when null;
recordingByValSites accounting as ByVal's; key immediate into edx).
- [ ] **Step 2:** TA tier: exact-kind guard, strict `cmp` length vs
Imm(K) → undefined path, null-data_ → undefined path, then the
SHARED tail with the constant-displacement Mem operand
(data_+offset_ base register, disp = K*width, no index register).
Tier selection from prior-version records (load predicate), TA
first, kind miss chains into the JSArray tier, specializedTAKind
recorded — mirror getByValImpl.
- [ ] **Step 3:** Tests (LITERAL keys + GetByIndex pins everywhere):
  - test/jit/getidx-conversions.js (ARCH-NEUTRAL): per-kind
    functions, warmed then re-invoked, NONZERO byteOffset views,
    constant indices covering 0, a middle value, and 255 where the
    view length permits; known bit patterns; Uint8Clamped; Uint32 >
    2^31; float NaN families via aliased integer views with
    `x !== x` AND `Object.is(x, NaN)` oracles (exercising the
    SHARED tail through the ByIndex address path);
    interpreter-diffed.
  - getidx-guards.js TA sections: the FULL equality-boundary matrix
    from the spec — K == length with a planted beyond-the-view word
    (inclusive acceptance reads it and fails by name), K > length,
    K == length-1, K == 0 on an attached zero-length view, K == 255
    on lengths 255 and 256; detached → undefined; Float16 + BigInt64
    decline and answer correctly.
  - getbyindex-inline-emitted.js gains: the indirect recording-call
    pin, the bounds-fail undefined path, and VERSION-2 EMISSION PINS
    FOR ALL NINE KINDS (tier line + constant-displacement access
    with its K*width displacement + one distinguishing convert per
    kind — Float64 pins its load + NaN-canonicalization sequence
    instead — SPEC-NEXT anchored). CONSTANT-SITE WARM-UP RECIPE
    (applies to these pins AND the conversions and boundary
    probes): a constant-key site cannot borrow another site's
    evidence, so EACH per-kind loader warms ITS OWN literal-K site
    with its intended kind — explicit -Xjit=force/-fno-inline/
    -Xjit-recompile-threshold flags, e.g. threshold 64 with 100
    separate calls per loader — and the known-value probe runs
    through a FRESH function entry after warm-up (installation
    swaps the function entry, never the running activation).
  - Policy: recompile-getidx-trigger.js (kind seen → version 2 +
    tier pinned; one poisoned order folded here or standalone —
    state which), recompile-getidx-duo.js (JSArray + Int32Array at
    ONE site: both tiers pinned, both values), recompile-getidx-
    demote.js (NumByValDemotions + values after the flip),
    recompile-getidx-demote-carried.js (demote a ByIndex site, then
    trigger a recompile from ANOTHER site in the same function —
    use the sibling's conditional PutById trigger
    (recompile-getval-demote-carried.js ~58) or another recording
    ByVal site; a GetById site alone supplies no decline crossings;
    short/long declining-suffix technique proves the slot survived
    — the null-only-init pin),
    recompile-getidx-budget0.js. NO delay/retired duplicates.
- [ ] **Step 4:** Prove-can-fail: (1) corrupt the SHARED tail's NaN
canonicalization — named checks in BOTH getval-conversions.js AND
getidx-conversions.js fail (the sharing proof, closing Task 1 Step
4); (2) TA bounds strict→inclusive — the K == length planted-word
case fails by name. Restore, REBUILD, green.
- [ ] **Step 5:** Full x86 suite + x86 jit + arm64 jit green.
Commit — `JIT: evidence-driven GetByIndex typed-array tier`

### Task 5: Validation, benchmark, docs

- [ ] **Step 1:** Rebuild + jit suite green on HV32, BOXED, MallocGC
(GetByIndex tiers are ungated — no new gatings expected; investigate
any new failure, do not gate it away without root cause), arm64
(host first), and one handle_san run (cmake-build-hs-hv32).
- [ ] **Step 2:** benchmarks/jit-benches/typed-array-index.js,
committed, per the spec: vec-dot-f64 (Float64Array length-4 vectors,
a[0]*b[0]+a[1]*b[1]+a[2]*b[2] hot loop), vec-dot-arr (dense arrays),
f16-idx and holes-idx controls with ACCUMULATING bodies (stated in
the header). A/B vs the pre-feature tip: candidate =
cmake-build-x86jit-rel rebuilt; baseline = throwaway worktree at the
pre-feature tip, Release configure mirroring the candidate's
CMakeCache toolchain/options; bytecode unchanged → compile ONCE with
the candidate compiler, `cmp` the baseline compiler's .hbc
byte-identical, run the shared artifact both sides; 3+3 runs,
medians, sequential, idle machine; remove the worktree after.
Bars (acceptance targets): vec-dot-f64 ≥ 1.2x; vec-dot-arr ≥ 1.1x;
controls ≥ 0.94x. A miss = measured diagnosis + explicit ruling,
never massaging.
- [ ] **Step 3:** doc/JIT.md: extend the GetByVal load-tiers passage
to cover ByIndex (constant-key twin, shared tail, fourth helper
identity); spec Delivered section with all runs/medians/evaluation.
Spec+plan stay uncommitted.
- [ ] **Step 4:** Full x86 suite once more; commit —
`JIT: GetByIndex tiers delivered (docs + benchmark)`

## Self-Review Notes (plan time)

- Spec coverage: factoring + byte gate (T1), recording/demotion
  identity (T2), JSArray tier + storage-state semantics (T3), TA
  tier + shared tail + boundary matrix + all-nine-kind pins + policy
  incl. carried (T4), validation/bench/docs (T5). Non-goals honored.
- Staging mirrors GetByVal: T2 inert; T3 tier-without-recording; T4
  recording+TA atomic (evidence always actionable).
- The two spec-critical hazards (begin-relative addressing; null-only
  slot init) appear in Global Constraints AND the owning tasks with
  their named tests.
- Delegated with instructions: helper naming/placement, poisoned-
  order placement, representative workload choice for the byte gate —
  each "match the sibling precedent and state which".

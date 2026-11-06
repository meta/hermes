# JIT PutByVal Monotone Emission + Demotion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the two invariant violations of the shipped PutByVal
shape feedback: remove the sticky-flag tax (monotone tier emission)
and end recording at hopeless sites (pointer-flip demotion), plus
poison-keeps-first-kind and the budget default change.

**Architecture:** Tier selection becomes static-plus-additive (JSArray
tier everywhere capability allows, typed-array tier on observed
evidence, nothing ever dropped); the helper call goes through a
per-site mutable slot that `considerRecompile` flips to the plain
helper when a site can no longer learn anything actionable; retirement
flips every slot of the retired record.

**Tech Stack:** C++17, asmjit (x86-64), lit/FileCheck.

**Spec:** doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md
(Codex-reviewed, all findings CLOSED; binding authority). Context:
2026-09-08-jit-putbyval-typed-array-design.md (the shipped design this
amends), dz 01a083c3 (strategy).

## Global Constraints

- x86-64 backend only; NO arm64 emitter changes; the arm64 BUILD stays
  green (shared driver/headers compile there; arm64 never creates
  records, so demotion is dormant).
- Tiers are never dropped; the JSArray tier is emitted wherever
  `HERMES_JIT_INLINE_SAFE_STORE` permits, unconditionally.
- Demotion is a data write, never a recompile; nothing un-flips a
  slot; retirement flips the retired record's slots.
- `kDemotionStableCrossings` is a constant (3), not a flag.
- Runtime C++ follows GC-safe rules: implementers touching
  lib/VM/JIT/JitHandlers.cpp or JitCompiler.cpp invoke the
  `gc-safe-coding` skill first.
- 80 cols, 2-space indent, doc comments on every declaration.
- Build: `cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-x86jit --target hermes`;
  arm64 check: `cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-arm64 --target hermes`;
  suite: `(cd /home/tmikov/work/hermes-x86-jit && LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes)`.
  Never `cd` bare; absolute paths or subshells.
- Every new/reworked lit test: copyright header, `// REQUIRES: jit`
  (+ `gc_hades` where the JSArray tier or ById progress is pinned),
  `// UNSUPPORTED: handle_san`, threshold pinned, prove-can-fail for
  headline pins.
- Commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB`.

## File Structure

- `include/hermes/VM/JIT/JitFunctionData.h` — record field changes,
  `recordingByValSites`.
- `lib/VM/JIT/JitHandlers.cpp` — recording rules, changed bit.
- `lib/VM/JIT/JitCompiler.cpp` — demotion pass, retirement sweep,
  progress simplification, carry-forward, install counting.
- `lib/VM/JIT/x86-64/JitEmitter.h/-property.cpp` — indirect call,
  monotone selection, sticky-flag deletion.
- `include/hermes/VM/RuntimeFlags.h`, `public/hermes/Public/RuntimeConfig.h`,
  `include/hermes/VM/JIT/{x86-64,arm64}/JIT.h`, `doc/JIT.md` — budget
  default.
- `test/jit/x86-64/` — reworked and new tests;
  `benchmarks/jit-benches/typed-array-store.js` — extended.

---

### Task 1: Record fields, recording rules, carry-forward

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h`
- Modify: `lib/VM/JIT/JitHandlers.cpp` (`recordByValObservation`)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (Compiler ctor carry-forward)

**Interfaces:**
- Produces: `JitByValSiteRecord` gains `uint8_t taPoisoned`,
  `uint8_t changed`, `uint8_t unchangedCrossings`, `void *helper`;
  `JitConsumerRecords` gains `uint32_t recordingByValSites`.
  (`jsArrayTierEmitted` and `kTAKindPoly` are DELETED by Task 3, not
  here — Task 1 is strictly additive.)

- [ ] **Step 1: Field changes in JitFunctionData.h**

Task 1 is STRICTLY ADDITIVE: do NOT delete `kTAKindPoly` or
`jsArrayTierEmitted` here — their users span recording, selection,
progress, and shipped test pins, and Task 3 retires all of them
atomically with the test rework. In `JitByValSiteRecord`, add, each
with a doc comment:

```cpp
  /// A supported kind DIFFERENT from taKind was observed. taKind keeps
  /// the first kind permanently; a poisoned site still gets its first
  /// kind's tier, and poison marks that no further kind can be added.
  uint8_t taPoisoned = 0;
  /// Set by recordByValObservation when a field actually transitions;
  /// cleared by the demotion pass each threshold crossing.
  uint8_t changed = 0;
  /// Consecutive crossings with no transition and nothing actionable;
  /// at kDemotionStableCrossings the site's helper slot is flipped.
  uint8_t unchangedCrossings = 0;
  /// The helper this site's emitted call goes through, as per-site
  /// MUTABLE state: initialized by the emitter to the recording
  /// _jit_put_by_val_{loose,strict}, flipped to the matching plain
  /// _sh_ljs helper on demotion. The emitted call is
  /// `movabs xScratch, &helper; call [xScratch]`. Never flipped back.
  void *helper = nullptr;
```

In `JitConsumerRecords`:

```cpp
  /// Number of sites whose helper slot still holds a recording
  /// helper. Gates the demotion pass constant-time once zero. 32-bit:
  /// site counts are bounded by bytecode size, which is 32-bit.
  uint32_t recordingByValSites = 0;
```

- [ ] **Step 2: changed-bit setting in recordByValObservation**

Task 1 keeps the SHIPPED recording semantics (including the
`kTAKindPoly` overwrite — the poison-keeps-first-kind change belongs
to Task 3, atomically with selection and the poly-budget test). This
step only adds `site.changed = 1` at every ACTUAL transition:
`jsArraySeen` 0→1, `otherSeen` 0→1, `taKind` None→K, and
taKind→`kTAKindPoly`. Same-shape declines set nothing.

- [ ] **Step 3: Carry-forward in the Compiler ctor**

The copy now: keeps observed fields AND `taPoisoned` AND the `helper`
slot VALUE (a demoted site stays demoted in the candidate; a
still-recording value is also fine to copy — the emitter only
initializes null slots); resets `changed = 0`,
`unchangedCrossings = 0`, `specializedTAKind = kTAKindNone` (as
shipped). `recordingByValSites` is NOT copied — it is computed at
install (Task 4).

- [ ] **Step 4: Build both backends; run the jit suite**

Expected: builds clean AND the full jit suite green — Task 1 is
additive, so any failure means an edit went beyond additive; fix it
here, do not defer.

- [ ] **Step 5: Commit** — `JIT: per-site helper slots and poison-keeps-first-kind records`

---

### Task 2: The indirect helper call

**Files:**
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` (declare the indirect call
  variant)
- Modify: `lib/VM/JIT/x86-64/JitEmitter.cpp` or `-internal.cpp`
  (implement it next to `callRuntimeWithSavedIP` — read that
  implementation first and mirror its saved-IP bookkeeping exactly)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (`putByValImpl`)
- Modify: `test/jit/x86-64/putbyval-inline-emitted.js`

**Interfaces:**
- Consumes: `JitByValSiteRecord::helper` from Task 1.
- Produces: `callRuntimeWithSavedIPIndirect(uint64_t slotAddr, const
  char *name)` — emits the saved-IP protocol around
  `movabs xScratch, slotAddr; call qword ptr [xScratch]`.

- [ ] **Step 1: Implement the indirect variant**

Read the real `callRuntimeWithSavedIP` in JitEmitter.cpp (~line 621)
first: it saves the IP, DELEGATES to `callRuntime` (which carries the
stack-alignment assertion, ~line 640), then conditionally INVALIDATES
the saved IP — it is not a symmetric save/restore. Mirror that
structure: add a `callRuntimeIndirect(uint64_t slotAddr, const char
*name)` sibling of `callRuntime` preserving its rspDelta/alignment
assertion, emitting
`a.mov(xScratch, asmjit::Imm(slotAddr)); a.call(x86::qword_ptr(xScratch));`,
and a `callRuntimeWithSavedIPIndirect` wrapper with the same
save/conditional-invalidate protocol. Doc comment states the contract:
the slot's current value is called; both possible callees follow the
saved-IP protocol; extra argument registers are ignored by the
4-argument callee under SysV.

- [ ] **Step 2: Wire putByValImpl**

Where the tail currently does the direct
`EMIT_RUNTIME_CALL(..., _jit_put_by_val_{strict,loose})`:

```cpp
  if (!site.helper) {
    site.helper = strict ? (void *)_jit_put_by_val_strict
                         : (void *)_jit_put_by_val_loose;
  }
  ...existing six-register argument setup unchanged...
  callRuntimeWithSavedIPIndirect(
      (uint64_t)&site.helper,
      strict ? "_jit_put_by_val_strict [indirect]"
             : "_jit_put_by_val_loose [indirect]");
```

(The null check is what preserves a carried demoted slot.) Keep the
typed-binding compile check: retain an `EMIT_RUNTIME_CALL`-style
`using`/assignment that type-checks both helper signatures without
emitting a direct call, or verify the existing pattern still
type-checks them; state in the report how the 6-arg binding stays
compiler-enforced.

- [ ] **Step 3: Update putbyval-inline-emitted.js**

The pin must match the actual INSTRUCTION, not the comment (the
comment is printed independently of what is emitted, so a
comment-only pin is vacuous): within the helper block, pin the
`mov r11, {{.*}}` materialization followed by
`call qword ptr [r11]`, bounded by the surrounding anchors. The
comment line may be pinned additionally, never instead.

- [ ] **Step 4: Build, run jit suite (green), commit** —
`JIT: call PutByVal helpers through per-site mutable slots`

---

### Task 3: Monotone selection, sticky-flag deletion, test rework

**Files:**
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (selection,
  `emitPutByValFastArrayTier` signature)
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` (declarations, delete flag
  plumbing)
- Modify: `include/hermes/VM/JIT/JitFunctionData.h` (delete
  `kTAKindPoly`, `jsArrayTierEmitted`)
- Modify: `lib/VM/JIT/JitHandlers.cpp` (poison-keeps-first-kind
  recording transition)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (`byValShapeProgress`,
  carry-forward reset removal)
- Modify/Delete tests as below.

- [ ] **Step 0: Retire the shipped poison/flag semantics atomically**

This task owns ALL of: deleting `kTAKindPoly` (recording switches to
taKind-kept + `taPoisoned = 1`, per the spec's table, with `changed`
on the poison transition), deleting `jsArrayTierEmitted` (its
declaration, the carry-forward reset, the progress clause, the
emission assignment), and every selection/progress consumer of both
names — together with the test rework below, in ONE commit, so the
suite is green at the boundary.

- [ ] **Step 1: Selection**

In `putByValImpl`:

```cpp
#if HERMES_JIT_INLINE_SAFE_STORE
  const bool emitJSArray = true;
#else
  const bool emitJSArray = false;
#endif
  const bool emitTA =
      site.taKind != JitByValSiteRecord::kTAKindNone &&
      isJitSupportedTypedArrayStoreKind((CellKind)site.taKind);
  site.specializedTAKind =
      emitTA ? site.taKind : JitByValSiteRecord::kTAKindNone;
```

(`taPoisoned` deliberately not consulted; `haveEvidence` and the
jsArraySeen selection disappear.) Delete the `stickyFlag` parameter
from `emitPutByValFastArrayTier` (both decl and impl), the flag-store
block at its tail, and the flag wiring. `targetKnownObject` and the
duo chaining stay exactly as shipped.

- [ ] **Step 2: Progress term**

`byValShapeProgress` keeps only:

```cpp
    if (s.taKind != JitByValSiteRecord::kTAKindNone &&
        isJitSupportedTypedArrayStoreKind((CellKind)s.taKind) &&
        s.taKind != s.specializedTAKind)
      return true;
```

Delete the `jsArraySeen && !jsArrayTierEmitted` clause and its
`HERMES_JIT_INLINE_SAFE_STORE` guard.

- [ ] **Step 3: Test rework** (each with prove-can-fail where the pin
is headline):

- `recompile-taval-byid-trigger.js`: invert
  `SPEC-NOT: // Inline fast array store` to a positive
  `SPEC: // Inline fast array store` AFTER the typed-array-store
  match (emission order: TA first); both-tiers is now THE pin.
- `recompile-taval-trigger.js`: NO JSArray pins added (portable,
  runs under MallocGC); typed-array pins unchanged; verify it still
  passes untouched or with minimal drift.
- `recompile-taval-object-drop.js` → rename
  `recompile-taval-object-keep.js`: the v2 window pins the JSArray
  tier PRESENT (`SPEC: // Inline fast array store`) and
  `1 observed, 0 specialized`; keep the ById trigger structure.
- `recompile-taval-mono-jsarray.js`: strip the flag rationale from
  comments; pins become: v2 keeps the array tier, specializes
  nothing. jsArraySeen now comes only from decline evidence, so the
  workload MUST produce at least one JSArray hole decline (store to
  an index past the dense region once), keeping the
  `1 observed, 0 specialized` pin (a zero-observed site prints NO
  diagnostic line at all — the shipped dump suppresses it — so the
  "pin 0 observed" alternative does not exist).
- `recompile-taval-duo.js`: keep; expectation unchanged (both tiers)
  — verify the pins still hold now that both tiers appear without
  flag evidence.
- `recompile-taval-poly-budget.js` → rework into TWO variants (can
  be one file with two functions or two files):
  Int32Array-first and Float64Array-first interleavings, each
  pinning `// Inline typed array store (kind <first kind's number>)`
  in the recompiled body; keep the phase-2 budget-preserved
  structure from the shipped test.
- DELETE `recompile-taval-selfcorrect.js` and
  `recompile-taval-delayed-site.js` (they pin removed machinery; the
  scenarios cannot occur under monotone emission).
- `taval-conversions.js`, `taval-guards.js`: run; expected
  unchanged. If the "1 observed, 1 specialized" pins drift, inspect
  and fix forward — do not weaken the diff RUN lines.

- [ ] **Step 4: Build both backends, full jit suite green, commit** —
`JIT: monotone PutByVal tier emission; delete the sticky flag`

---

### Task 4: Demotion pass, retirement sweep, counters

**Files:**
- Modify: `lib/VM/JIT/JitCompiler.cpp` (`considerRecompile`, install)
- Modify: the JitCounter enum + its name table (find via
  `grep -rn NumRecompiles lib/VM/JIT include/hermes/VM/JIT`)
- Create: `test/jit/x86-64/recompile-taval-demote.js`
- Create: `test/jit/x86-64/recompile-taval-demote-delay.js`
- Create: `test/jit/x86-64/recompile-taval-demote-carried.js`
- Modify: the two poisoned-order test variants from Task 3 (add the
  residual-demotion suffix + counter pins)

- [ ] **Step 1: The counters surface (verify, then use)**

The surface is `-Xjit-emit-counters` (see
test/jit/x86-64/counters-slow-call-kinds.js:8): final aggregate
counters print to STDERR at exit, formatted `Name: integer`
(lib/VM/JIT/x86-64/JIT.cpp, ~line 44; printed from
ConsoleHost.cpp:~1145). Verify these locations still hold, then pin
all new counter assertions through stderr FileCheck on that format.

- [ ] **Step 2: The demotion pass**

Add `JitCounter::NumByValDemotions` (+ name string). In
`considerRecompile`, after the staleness gate and BEFORE the
enabled_/budget early-outs, insert:

```cpp
  // Demotion pass (spec: "The demotion rule"). Needs no budget and
  // must run when no recompile can ever happen again.
  if (JitConsumerRecords *cr = vd->consumerRecords.get()) {
    if (cr->recordingByValSites != 0) {
      bool actionablePossible = enabled_ && data->recompileBudget != 0;
      for (auto &s : cr->byValSites) {
        if (!isRecordingHelper(s.helper))
          continue;
        bool observed = s.jsArraySeen || s.otherSeen ||
            s.taKind != JitByValSiteRecord::kTAKindNone;
        bool canProgress = actionablePossible &&
            s.taKind != JitByValSiteRecord::kTAKindNone &&
            isJitSupportedTypedArrayStoreKind((CellKind)s.taKind) &&
            s.taKind != s.specializedTAKind;
        if (!observed || canProgress || s.changed) {
          s.unchangedCrossings = 0;
        } else if (++s.unchangedCrossings >= kDemotionStableCrossings) {
          demoteSite(s);
          --cr->recordingByValSites;
          if (counters_.get())
            ++counters_.get()[(unsigned)JitCounter::NumByValDemotions];
        }
        s.changed = 0;
      }
    }
  }
```

with file-local helpers:

```cpp
/// Demotions per site require this many consecutive stable crossings.
static constexpr uint8_t kDemotionStableCrossings = 3;

static bool isRecordingHelper(void *h) {
  return h == (void *)_jit_put_by_val_loose ||
      h == (void *)_jit_put_by_val_strict;
}
/// Flip \p s to the plain helper matching its strictness, derived
/// from the slot's current value. Terminal: never flipped back.
static void demoteSite(JitByValSiteRecord &s) {
  s.helper = s.helper == (void *)_jit_put_by_val_strict
      ? (void *)_sh_ljs_put_by_val_strict_rjs
      : (void *)_sh_ljs_put_by_val_loose_rjs;
}
```

(`_sh_ljs_put_by_val_{loose,strict}_rjs` are real exported extern-C
symbols: declared around static_h.h:630, defined non-inline in
StaticH.cpp — take their addresses directly. Include JitHandlers.h,
which declares the recording helpers and brings in static_h.h.)

- [ ] **Step 3: Install-time counting and the retirement sweep**

In `compileCodeBlockImpl`'s install region: after filling the
candidate, compute `candidateVD_->consumerRecords->recordingByValSites`
by counting sites whose slot holds a recording helper (skip when no
records). When retiring the previous current, sweep ITS records:
every site with a recording slot → `demoteSite`, count into
`NumByValDemotions`? NO — retirement flips are bookkeeping, not
policy decisions; do NOT count them in NumByValDemotions (the tests
pin exact values). Set the retired record's `recordingByValSites` to
0. Doc-comment the sweep with the spec's "retirement IS the terminal
state" sentence.

- [ ] **Step 4: The three tests**

All use the counters surface from Step 1 plus the usual force/
threshold flags; decline arithmetic per the spec's schedule note
(fresh site: demotion lands AT the 4x-threshold crossing).

- `recompile-taval-demote.js`: one plain-object site, budget
  irrelevant (no progress possible), > 4 * 64 declines; pin
  `NumByValDemotions: 1` end-state and correct output.
  Prove-can-fail: MUTATION ONLY — bump kDemotionStableCrossings to
  255 in a scratch build and watch the `NumByValDemotions: 1` pin
  fail, then restore and REBUILD (a shortened-workload variant
  proves a different assertion and is not acceptable).
- Poisoned-residual demotion (extends Task 3's two poisoned-order
  variants, here because the counter now exists): give each variant
  a post-specialization suffix of >= 4 * 64 same-pattern declines
  (the residual kind's traffic through the specialized body, whose
  record is poisoned and unimprovable) and pin its demotion via
  `NumByValDemotions` end-state, both orders. Mind the schedule: the
  suffix's stable crossings only start once the record stops
  changing and any recompile has landed.
- `recompile-taval-demote-delay.js`: two RUN variants (A/B files or
  two functions): A = exactly the demotion schedule → pins
  `NumByValDemotions: 1`; B = same volume with the site's FIRST
  JSArray-hole decline injected one crossing before the deadline
  (a store to a holey array index through the same site) → pins
  `NumByValDemotions: 0`. The A/B asymmetry is the prove-can-fail
  for the `changed` branch.
- `recompile-taval-demote-carried.js`: drive a site to demotion,
  then an unrelated ById site triggers a recompile; two RUN lines
  with an identical prefix, suffixes differing only in post-demotion
  ByVal decline volume (no other decline source in the suffix), pin
  IDENTICAL `NumRecompileChecks` end-state values in both (exact
  numbers, run to discover them).

- [ ] **Step 5: Build both backends, full jit suite, commit** —
`JIT: demote hopeless PutByVal sites to the plain helper`

---

### Task 5: Budget default 1 → 2

**Files:**
- Modify: `include/hermes/VM/RuntimeFlags.h` (~line 250),
  `public/hermes/Public/RuntimeConfig.h` (~line 144),
  `include/hermes/VM/JIT/x86-64/JIT.h` (~line 236),
  `include/hermes/VM/JIT/arm64/JIT.h` (~line 233),
  `doc/JIT.md` (~line 697)
- Modify: `test/jit/x86-64/recompile-phased-warming.js` (pin
  `-Xjit-max-recompiles=1` on its RUN lines; its comments assume 1)

- [ ] **Step 1:** Change all five default sites together (grep for
the old default near each location to be sure nothing is missed:
`grep -rn "max-recompiles\|maxRecompiles\|MaxRecompiles" include public doc lib | grep -i "1\|default"` and inspect).
- [ ] **Step 2:** Pin phased-warming's budget; run the FULL jit suite
and inspect any other failure for implicit-default assumptions —
fix by pinning that test's budget explicitly, never by weakening
pins.
- [ ] **Step 3:** Build both backends, suite green, commit —
`JIT: default -Xjit-max-recompiles to 2`

---

### Task 6: Cross-mode validation, benchmarks, docs

**Files:**
- Modify: `benchmarks/jit-benches/typed-array-store.js`
- Modify: `doc/JIT.md`, the 2026-09-09 spec (Delivered section)
- dz: comment on 01a083c3

- [ ] **Step 1:** Heap modes: jit suite green on cmake-build-x86jit,
-hv32, -boxed; arm64 build; MallocGC build
(cmake-build-x86jit-malloc exists from the prior cycle — rebuild) and
manually run recompile-taval-trigger.js and recompile-taval-demote.js
against it (TA tier + demotion must work with no JSArray tier).
- [ ] **Step 2:** Extend `benchmarks/jit-benches/typed-array-store.js`
with three sections mirroring the existing two: `f16-store` (a
Float16Array fill — a permanently-declining site), `poisoned-store`
(alternating Int32Array/Float64Array fill, Int32-first), and keep
ta-store/arr-store unchanged. Update the header comment's recipe and
recorded numbers after measuring.
- [ ] **Step 3:** Perf acceptance is a SAME-WORKLOAD A/B against the
PRE-FEATURE base, exactly as the spec's thresholds are written.
Method: create a throwaway worktree at the pre-feature commit
`0006114a6` (`git worktree add <tmp> 0006114a6`), copy the EXTENDED
benchmark file into it, configure a minimal Release build there
(clang, same options as cmake-build-x86jit-rel), build `hermes`, and
measure all five sections (interp + -Xjit=force, 3 runs each,
precompiled with -O -emit-binary). Then rebuild
cmake-build-x86jit-rel at the candidate and measure identically.
Acceptance (spec thresholds, candidate vs pre-feature base on the
same machine and workload): arr-store candidate-JIT time >= 0.99x of
base-JIT (i.e. within 1%); f16-store >= 0.95x of base-JIT;
ta-store candidate-JIT >= 2.5x vs candidate interpreter (the win
does not regress); poisoned-store >= 1.2x vs candidate interpreter
in BOTH orders. Remove the worktree afterwards
(`git worktree remove --force <tmp>`). Record every number in the
report and the spec's Delivered section.
- [ ] **Step 4:** doc/JIT.md: update the PutByVal paragraph — monotone
emission, the helper-slot demotion, poison-keeps-first-kind, the new
budget default; point at the 2026-09-09 spec. Append the spec's
Delivered section. dz comment on 01a083c3: invariants I1/I2
delivered, with the measured rows; leave the issue open (it remains
the strategy home for the HC-cache direction).
- [ ] **Step 5:** Full non-JIT suite once
(`cmake --build cmake-build-x86jit --target check-hermes`), commit —
`JIT: document monotone emission and demotion; extend the store benchmark`

---

## Self-Review Notes (plan time)

- Spec coverage: monotone selection (T3), poison-keeps-first (T1+T3),
  slot + indirect call (T1+T2), demotion rule incl. budget-0/enabled_
  and empty-record skip (T4), retirement sweep (T4), counters gate
  (T1+T4), budget default with all five sites + phased-warming pin
  (T5), every reworked/deleted/new test named by the spec (T3+T4),
  perf acceptance rows (T6). Non-goals honored.
- Ordering keeps every task green: T1 fields compile-checked before
  use; T2 changes only the call mechanism (recording helpers still
  the targets, no behavior change); T3 changes selection + its tests
  atomically; T4 adds demotion + its tests; T5 and T6 are
  independent finishers.
- The plain-helper symbols are settled (real extern-C
  `_sh_ljs_put_by_val_{loose,strict}_rjs`, Task 4 Step 2); no
  delegated choice remains there.

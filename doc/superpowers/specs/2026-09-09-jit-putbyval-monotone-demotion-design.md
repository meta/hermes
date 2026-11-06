# JIT PutByVal monotone emission and slow-path demotion — design

Date: 2026-09-09. Status: implemented.
Amends: 2026-09-08-jit-putbyval-typed-array-design.md (tier selection,
instrumentation, recording rules, progress gate; this document
supersedes where they disagree). Strategy authority: dz 01a083c3
("per-site code-state strategy — learning must terminate") and its
discussion log.
Scope: x86-64 backend + shared driver; arm64 stays dormant.

## Motivation

The shipped PutByVal shape feedback wins 3x on typed-array stores but
violates both invariants of the code-state strategy, measured on the
per-kind fill benchmarks (base -> head, -Xjit=force):

    dense Array control  47->49 ms (0.96x)   -- I2: the sticky flag
    Float16Array fill   183->259 ms (0.71x)  -- I1: recording never ends
    poisoned            159->141 ms (1.13x)  -- order-dependent accident

I2 (instrumentation may live only on event-generating paths): the
sticky flag instruments the fast path, whose hits are silent, so
nothing can ever carry a pure-JSArray function out of the instrumented
state — the 2-4% tax is permanent for exactly the most common case.

I1 (every site must reach a terminal state whose code records
nothing): the fallback at every version is the recording helper, and
nothing ever selects the plain helper back — every permanently-
declining shape (unsupported kinds, plain objects, non-number values)
pays the recording delta forever.

Both follow from one asymmetry: the stateless fast path makes hits
unobservable, so tier-removal decisions can never be honest, and the
flag existed only to feed them. The fix removes the decisions instead
of the observability gap.

## Design

### Monotone emission: tiers are only ever added

Tier selection stops consulting evidence for the JSArray tier:

- The JSArray fast-array tier is emitted at EVERY site where
  `HERMES_JIT_INLINE_SAFE_STORE` permits one, in every version —
  exactly the pre-feature behavior. It is a static prior, never
  dropped, never instrumented.
- The typed-array tier is emitted at a site iff the record holds an
  observed supported kind (`taKind != kTAKindNone`), poisoned or not
  (see below). Since `taKind` never un-learns, selection from the
  observed field alone reproduces monotone emission; no
  previously-emitted set needs consulting.
- Both-tier sites keep the shipped chaining (typed-array tier first,
  kind miss into the JSArray tier, `targetKnownObject`).

Deleted outright: the sticky flag (emitter member, the
`emitPutByValFastArrayTier` stickyFlag parameter, the flag store),
the `jsArraySeen`-based selection, the drop-tier logic, and the
`jsArraySeen && !jsArrayTierEmitted` progress term (its premise — a
dropped tier — can no longer occur; MallocGC was already
capability-gated). `jsArrayTierEmitted` leaves the record.
`jsArraySeen` stays, recorded as today: it costs nothing, feeds
diagnostics, and its transitions participate in the demotion
stability rule below.

Consequence accepted: a pure-plain-object site carries a dead JSArray
tier (~6 cycles per store on a path that demotion sends to the plain
helper anyway; ~2% of the generic path). The `otherSeen`-only drop
exception from the 2026-09-08 spec is deliberately NOT kept — it was
the one non-monotone rule, and its failure mode (drop-and-discover at
secretly-mixed sites) bought only those cycles.

### Poison preserves the first kind

`kTAKindPoly` is deleted. The record keeps `taKind` = the FIRST
observed supported kind permanently, plus a new `uint8_t taPoisoned`
flag set when a DIFFERENT supported kind is observed. Recording rules
become:

- JSArray target → `jsArraySeen = 1` (as shipped; `changed` set on
  the 0→1 transition only);
- supported kind K, `taKind == kTAKindNone` → `taKind = K`;
- supported kind K, `taKind == K` → nothing;
- supported kind K, `taKind != K` → `taPoisoned = 1` (kind kept);
- unsupported typed-array kind, any other object, or a non-object →
  `otherSeen = 1`, as today.

Selection and progress ignore `taPoisoned`: a poisoned site still
gets (or keeps) its K1 tier, so the K1 half of its traffic runs
inline and the K2 half declines toward demotion. The poisoned
benchmark row stops depending on which kind happened to fill the
first threshold window. A second typed-array tier per site (PIC-style
growth) is explicitly out of scope; `taPoisoned` is what the demotion
rule needs to know the site cannot progress further.

The progress gate's ByVal term becomes exactly:

    taKind != kTAKindNone && isJitSupportedTypedArrayStoreKind(taKind)
        && taKind != specializedTAKind

(`specializedTAKind` keeps its shipped role: what THIS body emitted,
reset in carry-forward, filled at emission.)

### Slow-path demotion: the pointer flip

The recording helper's address stops being a code immediate and
becomes per-site mutable state:

- `JitByValSiteRecord` gains `void *helper`. The emitter initializes
  it (if still null after carry-forward) to `_jit_put_by_val_strict`
  or `_jit_put_by_val_loose` per the instruction, and emits
  `movabs xScratch, &site.helper; call qword ptr [xScratch]` through
  a new indirect variant of `callRuntimeWithSavedIP` — the same two
  instructions as the direct call plus one load, on a path that is
  slow by definition. The six argument registers are set up as today;
  a four-argument callee ignores r8/r9 under SysV, so one call
  sequence serves both targets.
- Demotion is a store: replace the slot's recording helper with the
  matching plain one (`_jit_put_by_val_strict` →
  `_sh_ljs_put_by_val_strict_rjs`, loose likewise — strictness is
  derived from the slot's current value, no extra field). From the
  next store on, the site calls the pre-feature helper directly: no
  recording, no counting, no extra hop. Single-threaded runtime,
  heap-resident slot: no code patching, no atomicity ceremony.
- Terminal is terminal. Nothing un-flips the slot; a demoted site's
  knowledge is frozen, consistent with the monotone-lattice stance.
  Carry-forward copies a demoted slot's value into the candidate, so
  demotion survives recompiles triggered by other sites (the
  candidate's own slot address is embedded in the new body as usual).
- Retirement demotes. The install step, when it moves `current` into
  `retired`, walks the retired record's sites and flips every
  still-recording slot to the plain helper. A retired body's
  observations influence nothing by construction (the staleness
  gate), so its recording is pure waste — and without this sweep a
  long-running old activation would record forever, since its own
  threshold crossings die at the staleness gate before any demotion
  pass could run. Retirement IS the terminal state for that body.

### The demotion rule

Two new record fields drive it: `uint8_t changed` (set by
`recordByValObservation` whenever a field actually transitions —
same-shape declines set nothing) and `uint8_t unchangedCrossings`.

`considerRecompile` gains a demotion pass, placed after the staleness
gate and BEFORE the budget check (demotion needs no budget and must
run at budget 0). On every threshold crossing, for each site whose
slot still holds a recording helper:

- if the record is empty (nothing ever observed), skip — the site's
  helper is not being called, and flipping would silence a site that
  might still start learning;
- else if the site can still progress (the ByVal progress term above,
  against the CURRENT body's `specializedTAKind`) AND a recompile is
  actually possible — recompile budget remains AND the JIT is still
  enabled — reset `unchangedCrossings` to 0. When no recompile can
  ever happen (budget 0, including the failed-compile zeroing; or
  `enabled_` false, e.g. another function hit the memory limit),
  every observed site is demotion-eligible: recording with no
  possible payoff is exactly what I1 forbids;
- else if `changed` is set, reset `unchangedCrossings` to 0;
- else increment `unchangedCrossings`; at
  `kDemotionStableCrossings` (constant, 3), flip the slot and count
  `NumByValDemotions`.

Clear every site's `changed` at the end of the pass. The rule needs
no shape taxonomy: "observed something, nothing new for three
crossings, nothing left to learn that we could act on" is the whole
terminal-state test. Sites still covered by it include hole-heavy
JSArray sites and same-kind-decline typed-array sites once their
tier is emitted — all of which stop paying the recording delta.

Cost: one deque scan per threshold crossing, on the slow path, only
while any site still records. The version record keeps a
`uint32_t recordingByValSites` count (site counts are bounded by
bytecode size, which is 32-bit; a 16-bit counter could wrap and
disable demotion), decremented on each flip; the
demotion pass is a constant-time skip once it reaches zero — ById
declines keep driving crossings after all ByVal sites are demoted,
so the gate, not the absence of crossings, is what bounds the cost.

### Observability for tests

`NumByValDemotions` joins the JitCounter enum and is reported through
the existing counters mechanism — final aggregate values printed at
exit (the surface counters-slow-call-kinds uses; the plan pins the
exact flag). There is no mid-run snapshot, so every counter-based pin
below is an END-STATE pin, and "recording stopped" is proven by an
A/B pair: two runs whose workloads differ only in post-demotion
decline volume must print IDENTICAL `NumRecompileChecks` (crossings)
— single-threaded determinism makes exact equality pinnable — with
all other decline sources (ById, other ByVal sites) held absent. The
"JIT ByVal sites: N observed, M specialized" dump line is unchanged
in format; its meaning under monotone emission is noted in
doc/JIT.md.

### Budget default

`-Xjit-max-recompiles` default changes 1 → 2. Budget exhaustion
already prevented later specialization in the shipped design;
demotion sharpens the consequence from "keeps recording uselessly"
to "recording ends and the site is frozen permanently" — a
still-recording site can be rescued by budget, a demoted one cannot.
Two recompiles cover the common case of one improvement per consumer
family (ById warmth; ByVal specialization) arriving at different
times, so fewer sites reach exhaustion still hungry. Cost: at most
one more retained body per function (reclamation remains the open dz
issue). The flag is unchanged. Implementation must move every stated
default together: RuntimeFlags.h, Public/RuntimeConfig.h, both
backends' JIT.h member defaults, and doc/JIT.md; and
recompile-phased-warming.js, whose comments assume a budget of 1,
pins `-Xjit-max-recompiles=1` explicitly to keep its scenario.

## Non-goals

- The HC-keyed per-site cache (guards-in-data) direction: waits on
  the parallel HC-tracking work; this design is compatible — the
  record is already shaped like a cache entry plus a helper slot.
- A Float16/F16C tier, Uint8Clamped, BigInt kinds (capability work,
  dz 01a0828c).
- Multiple typed-array tiers per site (PIC growth).
- Demotion for the ById recording helpers (same pattern, later).
- Any new command-line flag; `kDemotionStableCrossings` is a
  constant.
- Un-demotion / re-learning (graph edges); revisit only with
  evidence of phase-change workloads.

## Testing

Reworked (shipped pins that assert now-deleted behavior):
- recompile-taval-byid-trigger.js (gc_hades-gated): its version-2
  `SPEC-NOT: // Inline fast array store` INVERTS — monotone emission
  keeps the JSArray tier alongside the typed-array tier.
  recompile-taval-trigger.js stays PORTABLE (it deliberately runs
  under MallocGC, where no JSArray tier can exist): it keeps only its
  typed-array pins and gains no JSArray assertion; the monotone
  both-tiers pin lives in the gated test.
- recompile-taval-object-drop.js → repurposed (object-keep): the
  recompiled body KEEPS the JSArray tier at the plain-object site,
  `1 observed, 0 specialized`.
- recompile-taval-poly-budget.js → poisoned-keeps-first-kind: the
  alternating-kind site specializes the FIRST kind
  (`// Inline typed array store (kind 39)` for Int32-first), budget
  preserved via the existing phase-2 structure.
- recompile-taval-mono-jsarray.js: repurposed — v2 keeps the array
  tier, specializes nothing, without any flag machinery.
- recompile-taval-selfcorrect.js, recompile-taval-delayed-site.js:
  DELETED — they pin the flag/drop/self-correction machinery this
  design removes; the scenarios they guarded cannot occur under
  monotone emission.
- putbyval-inline-emitted.js: the helper call pin changes to the
  indirect form (`call qword ptr [r11]`).
- taval-conversions.js / taval-guards.js: unchanged (their pins are
  compile-time and their diffs are behavior-neutral to demotion).

New (note the schedule arithmetic: a fresh site's first crossing is a
`changed` crossing, so demotion of a site observed from cold takes
FOUR crossings — one changed plus kDemotionStableCrossings stable —
i.e. demotion lands AT the 4 * threshold crossing; declines beyond
that exercise the demoted path):
- Demotion end-to-end: a hopeless site (plain-object stores) driven
  through > 4 * threshold declines; pin `NumByValDemotions` = 1 in
  the end-state counters and unchanged program output. Prove
  can-fail by pinning the counter with demotion disabled.
- The `changed` reset delays demotion: A/B pair. Variant A: a
  plain-object site driven exactly through its demotion schedule —
  end-state pins `NumByValDemotions` 1. Variant B: identical decline
  volume, but a single NON-ACTIONABLE knowledge transition (the
  site's first JSArray hole decline, setting jsArraySeen — actionable
  progress would reset stability through the progress branch and
  prove nothing) injected one crossing before the deadline —
  end-state pins `NumByValDemotions` 0. Removing the `changed`
  branch makes B print 1; that asymmetry is the prove-can-fail.
- Poisoned-first-kind: TWO variants, Int32Array-first and
  Float64Array-first; each pins the specialization of ITS first kind
  (the rule is deterministic GIVEN the traffic order, not
  order-independent), plus demotion of the residual afterwards.
- Carried demotion: site demotes, an unrelated ById site triggers a
  recompile; recording-stopped proven by the A/B
  `NumRecompileChecks` equality oracle: the two runs share an
  IDENTICAL prefix (including the ById declines that trigger the
  recompile) and differ only in the volume of post-demotion ByVal
  declines in the suffix, where no other decline source may run;
  end-state crossing counts must be exactly equal.

Perf acceptance (per-kind fill benchmarks, the regressed rows):
dense Array control back to >= 0.99x of base; Float16 back to
>= 0.95x of base; typed-array rows unchanged (>= 2.5x); poisoned
each traffic order must meet the 1.2x threshold on its own (the
specialized kind differs by order; equality of the two speedups is
not required).

## Delivered (2026-09-09)

All five tasks landed: per-site helper slots and poison-keeps-first-kind
records (Task 1), the indirect helper call (Task 2), monotone tier
emission with the sticky flag deleted (Task 3), slow-path demotion with
the retirement sweep (Task 4), the `-Xjit-max-recompiles` default of 2
(Task 5). Cross-mode validation and the perf A/B (Task 6) follow.

**Cross-mode validation.** `jit/` lit suite (`LIT_FILTER='jit/'`, exit
0, 97 passed / 3 unsupported each): `cmake-build-x86jit` (HV64),
`cmake-build-x86jit-hv32` (HV32), `cmake-build-x86jit-boxed` (BOXED).
`cmake-build-arm64` rebuilds clean (arm64 stays dormant for live
recompilation; the tree builds and its own suite passes as documented
in doc/JIT.md). `cmake-build-x86jit-malloc` (MallocGC, no JSArray
tier) rebuilt and both ByVal recompile tests run manually against it:
`recompile-taval-trigger.js`'s three RUN lines (OUT/SPEC/OFF) and
`recompile-taval-demote.js`'s counters RUN line all pass -- the SPEC
dump shows only the typed-array tier (`// Inline typed array store
(kind 39)`, `JIT ByVal sites: 1 observed, 1 specialized`, no JSArray
tier line, as expected with `HERMES_JIT_INLINE_SAFE_STORE` off), and
the demote test prints `299` / `NumByValDemotions: 1` with
`NumRecompileChecks: 4`, confirming the typed-array tier and the
demotion pass both work with no JSArray tier in the picture at all.
Full non-JIT suite once on `cmake-build-x86jit`
(`check-hermes`): 4211 passed, 6 expected failures, 153 unsupported,
exit 0.

**Perf A/B**, exactly per the spec's method: a throwaway worktree at
the pre-feature commit `0006114a6` (removed after measuring), a
minimal Release build there (clang, `HERMESVM_ALLOW_JIT=2`) holding a
copy of the extended benchmark, measured against
`cmake-build-x86jit-rel` rebuilt at the candidate tip
(`7749341f8` + this task's docs/benchmark commit). Both `-O
-emit-binary`-precompiled to an identical-size `.hbc`
(3209 bytes -- same source, same bytecode version), 3 runs each of
interpreter and `-Xjit=force`, nothing else running on the machine.
3-run averages (ms):

    section              base-interp  base-JIT  cand-interp  cand-JIT
    ta-store                  245.3     242.0        260.0      63.7
    arr-store                 226.0      81.0        241.0      71.7
    f16-store                 307.7     311.3        312.0     273.7
    poisoned-store            242.7     236.0        257.3     150.0
    poisoned-store-rev        244.7     236.3        259.3     142.0

Acceptance, against the spec's thresholds:

    row               metric                    value   threshold  pass
    arr-store         base-JIT/cand-JIT         1.130x  >= 0.99x   yes
    f16-store         base-JIT/cand-JIT         1.138x  >= 0.95x   yes
    ta-store          cand-JIT vs cand-interp   4.084x  >= 2.5x    yes
    poisoned-store    cand-JIT vs cand-interp   1.716x  >= 1.2x    yes
    poisoned-store-rev cand-JIT vs cand-interp  1.826x  >= 1.2x    yes

All five rows meet or beat their threshold, with the two regressed
rows from the Motivation section actually reversed into wins on this
machine: dense-array control is 13% FASTER under the candidate than
the pre-feature base (I2 fixed -- no more sticky-flag tax on the
silent fast path) and Float16 is 14% faster (I1 fixed -- the
permanently-declining site now demotes to the plain helper instead of
recording forever). `base` here has no PutByVal shape feedback at all
(it predates 6fda06a94), so its ta-store/poisoned/f16 JIT columns are
essentially its interpreter numbers (no tier, no instrumentation);
the interesting comparisons for those rows are candidate-JIT against
candidate-interp, which is what the spec's thresholds use. The
~6% base-to-candidate gap visible in every interp column is uniform
across all five rows and does not affect any acceptance ratio (all of
which compare same-binary or JIT-to-JIT numbers).

Every individual run (not just the averages) is recorded in
`.superpowers/sdd/2026-09-09-jit-putbyval-monotone-demotion/task-6-report.md`.

dz 01a083c3 (the strategy issue both invariants trace back to) carries
these numbers and stays open as the strategy home for the HC-keyed
per-site cache follow-up.

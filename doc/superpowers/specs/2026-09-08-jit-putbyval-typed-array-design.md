# JIT PutByVal shape feedback and typed-array store tier — design

Date: 2026-09-08. Status: implemented.
Builds on: 2026-09-04-jit-recompilation-design.md and
2026-09-07-jit-version-data-design.md (the per-version record retrofit
this consumer was built for).
Tracked as: dz 01a07f0a-def8 (PutByVal typed arrays); the GetByVal
counterpart is dz 01a07f0a-11f4, out of scope here.
Scope: x86-64 backend + shared driver; arm64 stays dormant.

## Motivation

The inline PutByVal fast-array tier guards on CellKind::JSArrayKind
exactly, so every typed-array store declines to the runtime helper: a
C++ call with handle setup, virtual haveOwnIndexed + setOwnIndexed
dispatch, and a GCScopeMarkerRAII per element. Measured 2026-09-08
(Release/clang x86-64, precompiled bytecode, identical 1000x20000 fill
loops):

    arr-store (dense Array) interp ~227 ms  JIT ~81 ms   (2.8x)
    ta-store  (Int32Array)  interp ~246 ms  JIT ~240 ms  (~3%)

perf attributes ~70% of the JIT ta-store run to the per-store helper
chain. The emitted sequence that eliminates it is short, but its
conversion and store differ per element kind, so the site must be
specialized on the kind actually observed — which is exactly the
per-site, per-version feedback the JitVersionData retrofit was built
to carry. This is the first consumer of
JitVersionData::consumerRecords.

## Design

### Per-site shape records

`consumerRecords` stops being an ownerless `void *`: it becomes
`std::unique_ptr<JitConsumerRecords>`, destroyed with the version
record. `JitConsumerRecords` holds a container of per-site entries:

- `uint32_t siteId` — the PutByVal instruction's bytecode offset:
  unique within the function, stable across versions, known to the
  emitter at emission and passed to the helper as an argument (no
  return-address introspection, as always).
- `bool jsArraySeen` — JSArray traffic observed at this site.
- `uint8_t taKind` — none / a specific typed-array CellKind (mono) /
  poisoned (a second, different typed-array kind was seen).
- `bool otherSeen` — some other object kind declined here.
- `uint8_t specializedTAKind` + `bool jsArrayTierEmitted` — what the
  version's body actually emitted at this site, filled during
  emission; this is what the progress gate compares observations
  against.

The container must not relocate entries once created (a deque or
per-node allocation, not a SmallVector): version-1 code embeds the
address of a site's `jsArraySeen` flag directly. Lookup by siteId is a
linear scan; functions have a handful of ByVal sites.

Carry-forward: a candidate's records start as a copy of the observed
fields of the version it is compiled to replace (specialized fields
start empty and are filled by emission). The guarantee is: feedback
present in the current version's records at the compilation snapshot
is carried forward; observations made by retired bodies after their
retirement stay isolated in their own frozen records, exactly like
stale decline counts. Only the decline counter starts fresh. First
compiles start empty.

### The recording helper: _jit_put_by_val

The x86-64 PutByVal emitters (loose and strict; putByValWithReceiver
is unchanged) stop calling `_sh_ljs_put_by_val_{loose,strict}_rjs` on
their helper path and instead call new JitHandlers entry points
`_jit_put_by_val_{loose,strict}`:

    (SHRuntime *, SHLegacyValue *target, SHLegacyValue *key,
     SHLegacyValue *value, SHJitVersionData *vd, uint32_t siteId)

Six arguments, all in registers; `vd` and `siteId` are compile-time
constants materialized by the emitter, exactly like the ById helpers'
version-data argument. The helper's entry sequence, in order:

1. Classify the target and record the observation into `vd`'s entry
   for `siteId` (creating it if absent). Classification extracts
   scalars only — the object check and the cell-kind byte — and
   retains no raw pointer; recording allocates at most native memory
   and cannot GC.
2. The shared trigger tail: increment `vd->declineCount`, at
   threshold call `considerRecompile` (which may allocate and move
   the heap — hence recording first, so the observation that crosses
   the threshold participates in the feedback the recompile reads).
3. Forward to the store logic.

Counting semantics match `_jit_put_by_id`: every helper entry counts,
before any early path. The recording rules:

- target is a JSArray → set `jsArraySeen` (JSArray stores reach the
  helper when the inline tier declines — holes, boxed-double encodes,
  jumbo storage, write-barrier declines — or when the body emitted no
  JSArray tier at this site; all evidence of JSArray traffic);
- target is a typed array of a tier-supported kind → set `taKind`, or
  poison it if a different kind is already recorded;
- target is a typed array of an unsupported kind, or any other
  object, or a non-object → set `otherSeen`.

The forwarding boundary is the extern-C
`_sh_ljs_put_by_val_{loose,strict}_rjs` wrappers
(`putByValWithReceiver_RJS` itself is translation-unit-local to
StaticH.cpp), so behavior is unchanged by construction. On the
emitter side, `putByValImpl`'s four-argument function-pointer
parameter and bare `callRuntimeWithSavedIP` become an
`EMIT_RUNTIME_CALL`-typed six-argument binding; the bindings must
name the actual loose/strict helper symbols (EMIT_RUNTIME_CALL
stringifies its argument for dump diagnostics — a variable named
`shImpl` would print as `shImpl`). arm64 keeps calling the plain
`_sh_ljs` helpers:
no recording, no records, and the whole consumer is dormant there —
the same pattern as the ById cold-site machinery.

### Instrumentation: the sticky flag on prior-based tiers

Inline tier hits are otherwise invisible to the records (the tier
exists precisely so those stores never reach the helper), so every
**prior-based** JSArray tier — one emitted for an empty record, in
any version — instruments its success path: materialize the entry
address into the reserved scratch register (`xScratch`, the
loadBits64InGp pattern the ById tier uses for its version-data
pointer) and `mov byte ptr [reg], 1`. Two instructions, no
read-modify-write, no branches; one store to the record's cache line
per fast-path hit. A tier emitted on positive `jsArraySeen` evidence
carries no flag — the evidence already exists and carries forward.

Version 1's tiers are all prior-based, so version 1 is fully
instrumented; and a site that first executes only after a
ById-triggered recompile still has its (prior-based, instrumented)
tier, so its JSArray traffic is observed before any later version
could drop the tier. After a site has executed under an instrumented
tier, its record is complete for every shape that reached it.

A retired body still running on old frames writes its flag into its
own frozen record — harmless and isolated, the same story as stale
decline counts.

### Tier selection at recompile

Version 1 emits the unconditional JSArray tier everywhere it is
emittable (see capability gating below), exactly as today, plus the
flag store: that is a prior, not evidence, and code
that never recompiles behaves exactly as it does now. Recompiled
bodies emit, per site, only what the evidence supports:

- `jsArraySeen` → the JSArray tier (now by evidence);
- `taKind` mono → the specialized typed-array tier for that kind;
- both → both tiers, sharing the object check; each tier compares
  the cell-kind byte directly from memory, recorded typed-array kind
  first (it is what the site was observed declining on), with the
  typed-array tier's kind miss chaining into the JSArray tier. The
  duplicate one-byte compare on the JSArray branch is an accepted
  code-size/perf tradeoff over a register-held shared load;
- `otherSeen` only → no tier at all (drops today's dead JSArray tier
  at plain-object sites);
- empty record (site never executed) → keep the (instrumented)
  JSArray tier: absence of evidence keeps the prior and can never
  burn budget.

Capability gating: the JSArray tier exists only where
`HERMES_JIT_INLINE_SAFE_STORE` is nonzero (MallocGC builds disable
it). In MallocGC builds the prior is empty, `jsArraySeen` may still be
recorded but selects no tier, and the typed-array tier — which needs
no write barrier — remains available unconditionally on x86-64. This
requires moving `objectFlagsFastArrayMask()/Value()` out of
RuntimeOffsets.h's Hades-only section; they are GC-independent.

Tiers are dropped only on positive evidence of other traffic, never
on absence of evidence. The one guess this can get wrong — traffic
that changes shape after version 1 — is self-correcting: the dropped
tier's traffic now declines, is recorded, and re-qualifies the site
as progress for a further recompile, budget permitting. The records
converge toward the truth; every wrong guess is visible in them.

### Recompile policy integration

One counter, one budget: `_jit_put_by_val` shares the version's
declineCount, threshold (`-Xjit-recompile-threshold`), and budget
(`-Xjit-max-recompiles`) with the ById trigger. `considerRecompile`'s
ordering is unchanged (counter reset → staleness → budget); the
progress check gains a second source, either of which qualifies:

1. a recorded cold ById site whose cache has since warmed (as today);
2. a ByVal site whose observed shape is not covered by the current
   body's tiers AND is emittable: (`jsArraySeen` &&
   !`jsArrayTierEmitted`, only where the JSArray tier is emittable —
   see capability gating above) || (`taKind` mono && !=
   `specializedTAKind`).

A recompile triggered by either source emits everything both sources
know — warm ById tiers and shape-selected ByVal tiers in the same new
body. A poisoned `taKind` never counts as progress; same-kind
declines at a specialized site (out-of-bounds, non-number values)
bump the counter but can never re-qualify, so there is no
oscillation.

### The typed-array store tier

Emitted for one exact kind K; every guard declines to the recording
helper. Supported kinds: Int8/Uint8/Int16/Uint16/Int32/Uint32 (one
truncating-int path parameterized by store width), Float64, Float32.
Uint8Clamped (round-half-to-even), Float16, and the BigInt kinds
decline permanently and record as `otherSeen`.

1. Target is an object and its cell-kind byte equals K (only the
   is-object check is shared when both tiers are emitted; each tier
   compares the kind byte itself). This runs first so a duo site's
   kind miss chains into the JSArray tier rather than declining to
   the helper on a non-number value the JSArray tier could still
   handle.
2. Object flags pass the same masked compare the fast-array tier
   emits — `fastIndexProperties` set, `frozen` clear, via
   `objectFlagsFastArrayMask()/Value()` (each tier performs its own
   load-and-compare; the duplication at duo sites is part of the
   accepted no-shared-prefix tradeoff). Without it the tier would
   store where the helper throws: an out-of-range `defineProperty`
   clears `fastIndexProperties` and a subsequent `freeze` makes the
   indexed elements non-writable, so a strict-mode in-bounds store
   must throw, not write.
3. Convert the value into the register the eventual store will use,
   declining on anything that is not a number the kind can hold.
   There is no separate up-front "is a number" check: non-numbers
   are caught by the conversion itself — the sentinel compare for
   int kinds, the parity test for float kinds — which is safe here
   precisely because the kind dispatch and flags check already ran,
   so it can run after them without mishandling a value the JSArray
   tier could have taken instead.
   - int kinds: 64-bit `cvttsd2si`. For |x| < 2^63 this is exactly
     truncateToInt32's modular semantics; everything else (NaN,
     ±Inf, huge magnitudes, and any NaN-boxed non-number bit
     pattern) produces the 0x8000... sentinel — one compare,
     decline. (The helper computes those correctly, including the
     exact -2^63 input.)
   - Float64: the eventual store is the value's raw 8 bytes — a
     number's HermesValue bits are the double bits; the NaN-boxing
     parity test rejects non-number bit patterns before they would
     be read as a double.
   - Float32: `cvtsd2ss` converts the double; the same parity test
     as Float64 guards non-numbers before the conversion runs.
4. Key passes `emit_double_is_uint32` — the fast-array tier's exact
   sequence, including the parity-flag exit for NaN-encoded
   non-number keys.
5. Bounds: `idx < length_`, one 32-bit compare against the field. No
   0xFFFFFFFF special case is needed; length_ can never exceed it.
   (This tree has no resizable ArrayBuffers; length_ is fixed for the
   object's lifetime.)
6. Attached: load `buffer_` (compressed-pointer decode), load its
   `data_`; null means detached — decline. The check is free with a
   load the store needs anyway.

Once every guard passes, the store writes the value already converted
in step 3 at `data_ + offset_ + idx * width` (1/2/4 bytes for int
kinds, 8 for Float64, 4 for Float32).

No write barrier (typed-array storage holds no GC pointers), no
allocation, no GC interaction anywhere in the tier. The tier is
heap-mode-neutral: the only heap-encoding-sensitive steps are the two
compressed-pointer decodes it already shares with the fast-array
tier, so it works identically on HV64, HV32, and BOXED — and unlike
the fast-array tier it has no SmallHermesValue encode step to decline
on.

Every guard decline preserves the helper's existing behavior,
whatever that is for the case at hand — including strict-mode throws
for out-of-bounds stores on non-extensible typed arrays, prototype
setters for out-of-range indices, and ToNumber side effects for
non-number values. The tier never needs to know; it only declines.

### Diagnostics

The compile banner/dump gains an aggregate line when nonzero, in the
style of the cold-ById line: recorded ByVal sites, and how many the
body specialized (kind included at higher dump verbosity). This is
what makes tier selection pinnable in lit tests.

## Non-goals

- GetByVal (dz 01a07f0a-11f4): same records, separate work.
- Uint8Clamped / Float16 / BigInt64 / BigUint64 tiers.
- putByValWithReceiver, DefineOwnByVal, and every other indexed op.
- Changing the recompile budget default (1). A secretly-phase-changing
  site converges only with budget >= 2; revisit with data.
- arm64 emission. The driver-side records are shared; the arm64
  emitter records nothing and emits no tiers.
- Interpreter feedback into the first compile.

## Testing

New lit tests in test/jit/x86-64/ (force compilation, pinned
threshold, version checks scoped to the target function, prove-can-fail
for each headline pin per house rules):

- **Specialization end-to-end**: Int32Array fill loop; pin that
  version 2 reports the site specialized and the program output is
  unchanged.
- **Mono-JSArray**: a site whose v1 traffic is pure fast JSArray plus
  a ById trigger elsewhere in the function; pin that the recompiled
  body keeps the JSArray tier (flag evidence) and specializes no
  ByVal site.
- **Delayed first execution**: a ByVal site that first runs only
  after a ById-triggered v2, with JSArray-then-typed-array traffic;
  pin that v3 (budget 2) emits both tiers — proving v2's prior-based
  tier was instrumented.
- **Duo-morphic**: one site fed both a dense Array and an Int32Array
  before the first recompile; pin both tiers in version 2.
- **Poly stays dead, budget preserved** (budget 2): both typed-array
  kinds recorded before the first snapshot, plus an independent
  warmed-ById progress source producing v2; pin v2 specializes no
  ByVal site; then a positive later phase successfully installs a
  further version — proving the poly site burned nothing. (A bare
  "no version 3" pin would also pass on exhausted budget; the
  positive phase is what makes it meaningful, mirroring
  recompile-staleness-budget.js.)
- **Plain-object site**: with an independent progress source so a
  recompiled body exists; pin that it emits no tier at that site.
- **Self-correction**: phase change after v1 (budget 2): TA-only
  site gains JSArray traffic in v2; pin the restoring version 3 with
  both tiers.
- **Conversion semantics through the active tier**: NaN, ±Infinity,
  out-of-range doubles, negative zero, -2^63 into int kinds; Float32
  rounding; outputs equal to the interpreter's.
- **Flags-guard regression**: out-of-range defineProperty + freeze on
  an Int32Array, then a strict in-bounds store through a specialized
  site; pin the throw.
- **Observable fallback**: a strict out-of-bounds store on a
  non-extensible typed array through the specialized tier's decline;
  pin the throw. Genuine no-op cases (in-bounds detached store, plain
  out-of-bounds on an extensible target) pin unchanged output.
- Existing recompile suite passes unchanged. Expected test change:
  putbyval-inline-emitted.js pins the old helper symbol on the
  fallback path and updates to the new one.
- All three heap modes.

Perf acceptance: the ta-store microbenchmark (precompiled, Release)
under -Xjit=force improves from parity-with-interpreter to the same
order as the dense-array loop's JIT win (>= 2.5x vs interpreter).

## Delivered (2026-09-08)

Implemented as specified, with two rulings recorded during
implementation and reflected in the text above: the typed-array
tier's guard order (kind dispatch, then flags, then value conversion,
then key, bounds, attached) and the no-shared-kind-compare decision
at duo sites (only the is-object check is shared; each tier compares
the kind byte itself). See doc/JIT.md "ByVal shape records" for the
shipped description.

Perf: the ta-store microbenchmark improved 3.90x vs the interpreter
(252.3 -> 64.7 ms avg, Release/clang, precompiled bytecode); the
dense-array loop is unchanged.

Tests: test/jit/x86-64/recompile-taval-*.js, taval-conversions.js,
taval-guards.js. Verified on all three heap modes plus MallocGC;
arm64 remains dormant.

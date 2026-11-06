# JIT recompilation — design

Date: 2026-09-04. Status: approved design, pre-implementation.
Scope: x86-64 first; arm64 port after the shape settles.
Superseded in part by 2026-09-07-jit-version-data-design.md (metadata
and trigger sections).

## Motivation

The JIT compiles each function exactly once, so every specialization
decision is frozen at whatever moment the compile happened. The known
cost of that: a function compiled under `-Xjit=force` or a low
threshold has cold property caches, so the Get/PutById inline tiers —
which pin a hidden class from the cache at compile time — are never
emitted, and the function pays helper calls forever. The same
compile-once limit blocks every future feedback-driven specialization
(PutByVal shape pinning, call-site specialization), because feedback
worth acting on mostly arrives *after* the first compile.

This spec adds a generic recompilation mechanism: compiled functions
can be compiled again later, and the new body installed for future
invocations. The mechanism is the deliverable; its first consumer is
deliberately the smallest one that exercises every piece.

## Non-goals

- **No deopt, no speculation.** Every emitted specialization remains
  guarded with a helper fallback, exactly as today. All versions of a
  function are semantically interchangeable — a recompile changes
  performance, never behavior. Nothing ever needs to be invalidated.
- **No OSR.** A new version takes effect on the next invocation. A hot
  loop inside a single long-running activation keeps its old body.
- **No async or background compilation.** Recompiles are synchronous,
  like today's threshold compiles. The install interface
  (`setJITCompiled`) does not care who calls it, so this can change
  later without redesign.
- **No reclamation of retired code** in v1 (see Lifetime below).

## Background: what the codebase already provides

Three verified facts make the mechanism small:

1. **Every call already goes through a per-call indirection.** JIT
   calls load `CodeBlock::JITCompiled_` and call it each time
   (`mov rdx,[codeBlock+...]; test; call rdx`); the interpreter checks
   the same field per invocation. Installing a new version is one
   plain pointer store — no code patching, no call-site tracking, and
   stale callers self-heal on their next call.
2. **The write slow paths already land in C++ with the CodeBlock in
   hand.** `_jit_put_by_id` and its variants take `SHCodeBlock *` as
   an argument, so triggers and counters can live entirely in those
   helpers: zero emitted instrumentation in v1. (The GetById slow
   path is different: the JIT calls the shared SH helper
   `_sh_ljs_get_by_id_rjs`, which has no CodeBlock parameter and is
   also called from shermes-AOT code, so it cannot carry a trigger —
   see the trigger section.)
3. **Compiled code is never freed today.** `JITContext::Impl` owns the
   asmjit runtime; code lives until the context dies. Keeping retired
   versions alive is the status quo, not a new leak.

Also relevant: `JITContext::shouldCompile()` has the precondition that
`JITCompiled_` is null — today's flow compiles at most once, and
recompilation must enter through a new explicit entry point rather
than by loosening that.

## Design

### Per-function metadata: `JitFunctionData`

Allocated lazily at first compile; a new `CodeBlock` field
(`JitFunctionData *jitData_`, `HERMESVM_JIT` builds only) points to
it; freed with the CodeBlock. v1 contents:

- `uint8_t recompileBudget` — initialized from
  `-Xjit-max-recompiles`; decremented per recompile; 0 disables all
  triggering for this function.
- `uint16_t coldByIdSites` — derived by the compile driver at install
  as the clamped sum of the two vectors below: the number of
  Get/PutById sites that declined tier emission because their
  property cache was cold. This is the count of sites a recompile
  could actually improve, which is what keeps the trigger precise.
- `llvh::SmallVector<uint8_t, 4> coldWriteCacheIdxs` /
  `coldReadCacheIdxs` — the write- and read-cache indices of those
  sites, moved out of the emitter's own per-compile vectors at
  install. `considerRecompile`'s progress check (below) walks these
  to require that one of these SPECIFIC sites, not just any cache,
  has warmed.
- `uint32_t declineCount` — bumped by the ById helpers; reset at each
  install.
- `llvh::SmallVector<JITCompiledFunctionPtr, 1> retired` — prior
  bodies. Bookkeeping only (dump labeling, sanity); the code itself
  is owned by `JITContext::Impl` regardless.
- An opaque pointer reserved for consumer-specific feedback records
  (null in v1; the PutByVal consumer will hang its per-site records
  here).

The emitter's only new obligation is recording, per Get/PutById site,
the cache index whenever tier emission is skipped for a cold cache —
into its own `coldWriteCacheIdxs_`/`coldReadCacheIdxs_` vectors. It
emits no instrumentation.

### Swap protocol and lifetime

New entry point `JITContext::recompile(Runtime &, CodeBlock *)`:

1. Run the ordinary compilation pipeline. It reads the *current*
   property-cache state, so tiers skipped by the previous compile are
   emitted now.
2. On success: push the old pointer onto `retired`, reset
   `declineCount`, decrement `recompileBudget`, then
   `setJITCompiled(new)`.
3. On failure (e.g. `-Xjit-memory-limit` reached): zero the budget —
   never retry a function that cannot compile.

The VM is single-threaded per Runtime, so the install is a plain
store. Activations already executing the old body finish on it; the
old body stays valid (owned by Impl) and stays correct (fully guarded,
helper-complete) indefinitely. Memory accounting needs nothing new:
every compile already counts against the JIT memory limit, and
retired bodies are counted by construction. The per-function budget
(default 1) bounds total growth.

"Finish on the old body" holds without exception, string switches
included: the RuntimeModule's shared string-switch tables map a case
string to a case *index*, never to a code address, and each compiled
body dispatches through a jump table of its own, emitted next to the
switch. A successful recompile therefore mutates no RuntimeModule
state at all, and a switch executed in an old activation lands in
that activation's own body.

Reclamation of retired bodies is intentionally deferred; the
follow-up (conservative native-stack scan at a quiescent point,
freeing versions no frame references) is filed as a dz issue.

### Trigger and v1 policy

The `_jit_put_by_id` helper family gets a short trigger at helper
ENTRY — before any raw object pointer is derived (recompilation may
allocate) and before the early-return cache-hit paths, so every
decline is counted — touching only non-GC state:

    if (data && data->recompileBudget &&
        ++data->declineCount >= kRecompileDeclineThreshold)
      considerRecompile(runtime, codeBlock);

`considerRecompile()` spends budget only when progress is possible:
`coldByIdSites > 0` and one of the SPECIFIC sites the last compile
recorded as cold (`coldWriteCacheIdxs` / `coldReadCacheIdxs`) has
since warmed — a write site when its cache now names a class, a read
site only when it clears the specialization gate (`numGoodChanges ==
1` and a class recorded); an unrelated warm cache does not count. The
scan (executed once per threshold crossing, not per decline) walks
only the recorded indices, not every cache entry. If the check
passes, it calls `recompile()` synchronously right there; compilation
never executes JS, so there is no reentrancy. If the check fails —
the declines come from polymorphic guard misses on tiers that were
already emitted — it resets `declineCount` and keeps the budget; the
steady-state cost of a polymorphic function is one cheap scan per
`kRecompileDeclineThreshold` declines.

`kRecompileDeclineThreshold` is a named constant, initially 64.

The v1 trigger listens on the write side only, because only the put
helpers receive the CodeBlock. The recompile it triggers still warms
GetById tiers too — the pipeline re-reads all caches. A function
whose only cold sites are reads and which performs no puts will not
trigger in v1; the easy widening is a thin JIT-specific GetById
wrapper that takes the CodeBlock and delegates to the SH helper, at
the cost of one small mirrored emitter change. That is the first
follow-up, not part of v1.

### v1 consumer: ById cache warming

No new tier code. A function compiled with cold caches (force,
threshold 0, or simply compiled before the relevant code path ran)
currently never gets the Get/PutById inline tiers. With the mechanism,
its helper traffic crosses the threshold, the warm-scan passes, and
the single recompile emits the tiers the first compile could not.
This closes a known gap and exercises every mechanism piece: metadata,
emitter reporting, helper trigger, policy, synchronous recompile,
install, retirement.

## Flags and observability

- `-Xjit-max-recompiles=N` — per-function budget. Default 1.
  `0` restores exactly the current compile-once behavior.
- Counters (existing JitCounters block, runtime-side, no emitted
  code): `NumRecompiles`, `NumRecompileChecks` (threshold crossings
  that ran the warm-scan).
- `-Xdump-jitcode` prints recompiles as
  `JIT compilation of FunctionID N, 'name' (version 2)`; perf jitdump
  symbols get a ` v2` suffix so profiles never conflate versions.

## Testing

- **Headline lit test / prove-can-fail**: compile under `-Xjit=force`;
  the dump shows a ById site without its tier. Loop until the caches
  warm and declines cross the threshold; the dump shows the
  `(version 2)` body with the tier, pinned under the existing
  `SPEC`/`%hv-mode` scheme. A second RUN line with
  `-Xjit-max-recompiles=0` CHECKs that no version 2 appears — the
  mechanism's absence is observable.
- **Old-body-on-stack**: a recursive function crosses the decline
  threshold deep in the recursion; the recompile installs mid-flight
  and the outer frames unwind through the retired body. Behavioral
  test on the ASan tree.
- **Determinism**: run the headline test twice and diff the dumps.
- **Suites**: full jit lit suite and Octane behavioral runs under
  `-Xjit=force` with budget 1, on the ASan HV64, HV32, and BOXED
  trees, plus the handle-san tree.
- **Existing gates unchanged**: the current `-emitted` pin tests use
  `-Xjit-threshold=4`, where caches are warm at first compile —
  `coldByIdSites == 0`, so no trigger fires and their dumps do not
  change. The byte-identity baseline workflow (`jit-dump.sh`) adds
  `-Xjit-max-recompiles=0` to its canonical flags so baselines stay
  single-body by construction.

## Forward path

- **arm64**: shared code throughout except the emitter's cold-site
  recording into `coldWriteCacheIdxs_`/`coldReadCacheIdxs_` — a small
  mirrored change; port commit after x86-64 settles.
- **PutByVal shape pinning** (second consumer): per-site
  observed-kind records hung off the reserved pointer; the emitter
  assigns site ids and passes one extra immediate argument to the
  PutByVal helper (that consumer's only mirrored emitter change).
  Same policy shape: consistent kind + declines over threshold +
  budget → recompile with the pinned arm.
- **Later, behind the same interfaces**: call-site specialization,
  tiered compile budgets (cheap first compile, careful recompile),
  async install, retired-code reclamation.

## Delivered (2026-09-05)

Implemented on x86-64 per this spec; see doc/JIT.md "Recompilation".
Tests: test/jit/x86-64/recompile-*.js. arm64 carries the setter/getter,
the `recompile`/`considerRecompile` declarations with their live shared
definitions, the dormant `coldWriteCacheIdxs_`/`coldReadCacheIdxs_`
Emitter members, and the live-but-inert helper trigger; no
recompilation happens there because the arm64 emitter never records
cold sites (arrives with the arm64 port).

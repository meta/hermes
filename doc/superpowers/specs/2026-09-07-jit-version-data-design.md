# JIT per-version feedback records — design

Date: 2026-09-07. Status: approved design, pre-implementation.
Reviewed externally (Codex) 2026-09-07; all findings incorporated.
Amends: 2026-09-04-jit-recompilation-design.md (metadata and trigger
sections; this document supersedes where they disagree).
Scope: shared driver + both backends' call emission for the recording
helper; behavior-neutral for everything the existing tests pin.

## Motivation

The recompilation mechanism's feedback today is version-agnostic: the
property caches are ground truth about the program, and the per-function
metadata holds only a snapshot of them plus one counter. That stops
being sufficient the moment the JIT records observations of its own —
per-site kind sets for polymorphic accesses, decline reasons, the
planned PutByVal shape records. Such observations are facts about what
reached a PARTICULAR BODY's slow paths: a kind that reaches version 1's
helper may be handled inline by version 2 and never reach its helper at
all. Blending streams from two versions distorts the picture the next
recompile decides on.

Today a helper cannot even tell which version called it: it receives
the CodeBlock, which is per-function. This retrofit makes each compiled
body self-describing before the first rich-feedback consumer is built
on the wrong foundation.

## Design

### JitVersionData: one record per compiled body

One record describes one compiled body:

- `CodeBlock *codeBlock` — back-pointer; the record is the helper's
  sole identity argument, and everything function-level is reached
  through this.
- `JITCompiledFunctionPtr body` — the version's entry point, set at
  install. The retired-code bookkeeping merges into the records: a
  version IS {its code, its observations}.
- `uint32_t declineCount` — this body's own disappointment counter.
- `uint16_t coldByIdSites` and the two cold-index vectors
  (`coldWriteCacheIdxs` / `coldReadCacheIdxs`) — move here from
  JitFunctionData; they always described one compile's output.
- `void *consumerRecords` — moves here; future per-site records are
  version-local by construction.

`JitFunctionData` keeps only control state: `recompileBudget`,
`version`, `std::unique_ptr<JitVersionData> current`, and
`llvh::SmallVector<std::unique_ptr<JitVersionData>, 1> retired`
(replacing the bare retired-pointer vector). Its allocation point does
NOT move: it is created at first successful install, as today, which
keeps the existing version-numbering computation valid (the driver's
pre-emission local, `priorData ? priorData->version + 1 : 1`, continues
to feed the banner, the success line, and the perf-symbol suffix; the
candidate record itself needs neither budget nor version).

### Candidate ownership and the longjmp boundary

The candidate record is allocated before emission begins and is owned,
until install, by a `Compiler` MEMBER (`std::unique_ptr`), following
the `exceptionHandlers_` precedent: the emitter's error path leaves
compilation by `longjmp`, which skips local destructors, while the
`Compiler` object itself lives outside the jump boundary and is
destroyed normally. The member must be declared (and thus initialized)
before `em_` if the emitter receives the pointer at construction. The
emitter only borrows the raw pointer to embed its address. Both failure
paths — the memory-limit null return and the longjmp error path —
destroy the candidate with the `Compiler` and never touch `current` or
`retired`. A record whose compile fails is discarded with the
never-installed body; nothing observed it.

Lifetime invariant (this is the requirement, not a mechanism claim): a
version record must outlive every possible execution of its body —
future calls, which hold the CodeBlock alive through the closure's
RuntimeModule reference, and already-running activations of retired
bodies alike. Records die with the CodeBlock's `JitFunctionData`; the
machine code, owned by `JITContext::Impl`, may harmlessly outlive the
records once nothing can execute it.

### The install protocol (shared by first compiles and recompiles)

At the point where `compileCodeBlockImpl()` installs today, in order:

1. `ensureJitData(getMaxRecompiles())` — budget initialized on first
   allocation only, as today.
2. Fill the candidate: cold lists from the emitter's vectors, derived
   `coldByIdSites`, `body` = the new entry point.
3. Move `current` (if any) into `retired`; move the candidate into
   `current`.
4. `setJITCompiled(current->body)` — the actual publication; without
   this step `current` would describe a body calls never enter.

`recompile()` keeps only its policy role — budget decrement, `version`
bump, counters — and failure handling (budget zeroed on a failed
compile). The record swap and the pointer publication happen together
in the single-threaded install, and the scan-then-install ordering
inside one helper call means no reader ever observes the swap midway.

### The helper contract: pass the version record, not the CodeBlock

The recording-helper family (`_jit_put_by_id` and variants; later the
PutByVal helper) replaces its `SHCodeBlock *` parameter with an opaque
`SHJitVersionData *`. This is compression, not addition: the emitted
call today materializes the CodeBlock as a compile-time immediate, and
the record's address is a compile-time constant of the same kind — the
emitters swap which constant they embed. Argument count and emitted
shape are unchanged on both backends. The helper recovers the CodeBlock
with one load through the back-pointer.

Helpers that take a CodeBlock for non-recording reasons are unchanged.
The signature swap is compiler-enforced through `EMIT_RUNTIME_CALL`'s
typed binding: a backend left on the old signature does not build.

No return-address introspection anywhere: identity travels only by
argument.

### Trigger and staleness

The existing ENTRY trigger in `_jit_put_by_id` is replaced in place: it
remains the helper's first statement, before any raw object pointer is
derived (recompilation may allocate and move the heap) and before every
early return (cache hit, cached add), so all declines are counted:

    if (LLVM_UNLIKELY(
            ++vd->declineCount >=
            JitFunctionData::kRecompileDeclineThreshold))
      runtime.getJITContext().considerRecompile(runtime, vd);

`considerRecompile(Runtime &, JitVersionData *)` resets the counter,
then applies the gates in order:

1. **Staleness**: `vd != vd->codeBlock->getJitData()->current.get()` →
   return. A retired body's events land in its own frozen record and
   influence nothing; "old versions do nothing" is one pointer compare,
   with no filtering logic in the hot tail. (Consequence, intended:
   stale-frame declines no longer count toward the current version's
   threshold at all — before this change they did, harmlessly but
   noisily.)
2. Budget, cold-site presence, and the per-site warm scan as today,
   reading the cold lists from `vd` (== current).

After budget exhaustion the current record's counter keeps wrapping
through the threshold, costing one early-out call per threshold
crossing; this replaces the old entry check's budget test and is
negligible on a slow path. arm64 is uniform, not special-cased: its
driver allocates records and its bodies pass them; with no cold sites
ever recorded, gate 2 keeps it dormant exactly as before.

## Non-goals

- No reclamation change; see the lifetime invariant above.
- No threshold flag (tracked separately, dz 01a077ac).
- No new consumers: `consumerRecords` moves but stays null. The PutByVal
  work builds on this foundation instead of migrating off the old one.

## Testing

The existing recompile suite pins the observable behavior and must pass
unchanged: threshold semantics for the current version are identical,
and no pinned test depends on stale-frame declines counting toward the
current threshold. The `-emitted` pin tests match the call setup with
unpinned immediates, so the swapped constant changes no CHECK line.

One new two-phase lit test pins the staleness gate where it IS
observable (force compilation, inlining disabled, depth 100, budget 2,
one object across both phases, all version checks scoped to the target
function):

- The recursion stores to a distinct pre-existing property `q` on the
  UNWIND path (descent after the mid-recursion install runs v2; only
  the unwind executes in v1). `q`'s cache is cold at both v1's and
  v2's compiles, and the unwind's helper calls warm it.
- **Phase 1** (the deep call): the unwind produces ~37 v2 declines and
  ~64 v1 declines. Under the OLD shared counter those 64 stale
  declines cross the threshold with `q` warmed and produce version 3
  during the unwind; with per-version counters they land in the
  retired record. Pin: NO version 3 before a phase marker printed
  after the deep call returns.
- **Phase 2** (shallow re-invocations): ~27 more calls push v2's own
  counter across the threshold. Pin: version 3 DOES appear, with `q`'s
  tier — proving the budget survived phase 1 and that triggering still
  works (without this phase, the phase-1 pin would also pass if
  recompilation were accidentally disabled).

Prove-can-fail by demonstrating the phase-1 pin trips under the old
shared-counter behavior or a deliberate local mutation.

## Delivered (2026-09-07)

Implemented as specified; see doc/JIT.md "Recompilation" and
test/jit/x86-64/recompile-staleness-budget.js (the two-phase pin).
Both backends pass the record; arm64 remains dormant (empty cold
lists).

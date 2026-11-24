# JIT: box doubles inline in the store tiers

dz 01a185d0-5d43 (HV32 PutByVal), with the PutById side of the same
cause recorded on dz 01a04e00-0811 (Box2D).

## Problem

Under `HERMESVM_BOXED_DOUBLES` (the HV32 and BOXED heap modes) a heap
slot holds a SmallHermesValue, so both inline store tiers encode the
value before storing it. A double the encoding cannot hold inline must
become a heap `BoxedDouble`, and the encoder declines it to the C++
helper instead (x86-64 `JitEmitter-internal.cpp:218-224`, arm64
`JitEmitter-internal.cpp:197-207`). Under HV32 only doubles whose low 35
bits are zero encode inline (`SmallHermesValue.h:248-250, 323-337`):
integers up to 2^18 in magnitude, 0.5, -0.25; not 0.1, not 2^28-1.

On numeric code nearly every store declines. Instrumented x86-64 runs
(dz 01a185d0-5d43, 2026-10-29 log entry):

| bench | HV32 helper share of PutByVal | of which boxing | HV64 helper calls |
|---|---|---|---|
| NavierStokes | 94.4% (63.7M of 67.5M) | 95.4% | 0 |
| Crypto | 89.6% (96.2M of 107.3M) | 99.1% | 2.1M (structural) |

The helper path is `putByValWithReceiver_RJS` -> virtual
`haveOwnIndexed`/`setOwnIndexed` -> `_setOwnIndexedImpl` ->
`BoxedDouble::create`: 34.9% of NavierStokes' samples and 26.6% of
Crypto's under HV32, against under 1% under HV64. JIT/interp speedup
falls from 2.89x to 1.52x (NavierStokes) and 2.37x to 1.85x (Crypto).
The PutById tier uses the same encoder (x86-64
`JitEmitter-property.cpp:2373-2374`, arm64 `:2552-2553`): Box2D
declines 63.6% of its inline-tier entries under HV32 (dz 01a04e00-0811),
1.92x -> 1.25x.

## Goal and success criteria

The two inline store tiers store a double that needs a box without
leaving JIT code: they allocate the `BoxedDouble` in the young
generation, write the double into it, and store the tagged pointer.

1. Under HV32 and BOXED, an inline-tier store that today declines only
   because the value needs a box completes inline, unless the young
   generation is full. Evidence: the new per-tier box counters match the
   tests below, and the boxing share of helper calls on
   NavierStokes and Crypto (re-measured with the dz 01a185d0-5d43
   instrumentation) is the young-generation-full fallbacks only.
2. Under HV64 the emitted code of both tiers is unchanged. By
   construction: every edit to a path HV64 compiles is confined to code
   under `HERMESVM_BOXED_DOUBLES`. Checked: the `-Xdump-jitcode` output of
   the four emitted-code tests' inputs and of NavierStokes and Box2D,
   before and after, is identical on the HV64 tree once absolute
   addresses (runtime, helper and code pointers) are normalized. The
   directives active under HV64 also stay verbatim and in order, which
   alone would not prove identity: they match selected instructions with
   wildcards.
3. The tests below pass on x86-64 HV64, HV32 and BOXED (ASan Debug,
   `HERMES_SLOW_DEBUG`) and on arm64 HV64, HV32 and BOXED under qemu.
4. Octane under HV32 is reported for every benchmark (method under
   Benchmarks); no HV64 benchmark changes beyond noise.

## Non-goals

- Stores declined because old-generation marking or a compaction is
  active (`emitSafeStoreOrSlow` steps 3-4 below). They remain helper
  calls; on NavierStokes 36-39% of the box stores would still take this
  decline (dz 01a185d0-5d43). A separate change, measured after this one.
- Object literals with a non-inline double: the fast path is refused at
  compile time (x86-64 `JitEmitter-object.cpp:243-248`, arm64 `:238`).
- `HERMESVM_SANITIZE_HANDLES`: the encoder's Handle-San arm keeps
  declining every number (x86-64 `JitEmitter-internal.cpp:196-213`,
  arm64 `:169-186`). The runtime forces a young-generation collection on
  every allocation in that mode (`HadesGC.h:1652-1661`), which an inline
  allocation would skip.
- Allocation instrumentation. Inline allocation skips `GCBase::newAlloc`
  (allocation-location tracker, sampling allocation profiler; only under
  `HERMES_MEMORY_INSTRUMENTATION`, `GCBase-inline.h:97-99`), exactly as
  the existing inline allocators of objects and environments already do
  (x86-64 `JitEmitter-object.cpp:47-53, 180-186, 343-372`,
  `JitEmitter-env.cpp:64-70, 145`). Nothing in `lib/VM/JIT` is gated on
  it today; this change does not add the first gate.
- A per-site decline reason. A young-generation-full fallback counts as
  a decline like any other (see Decline accounting).
- Typed arrays, `FastArrayStore`/`FastArrayPush`, environment slots:
  none stores a SmallHermesValue through an inline encoder (typed arrays
  store raw elements, x86-64 `JitEmitter-property.cpp:206-392`; the fast
  array ops are helper calls, `JitEmitter-array.cpp:265-298`; environment
  slots are 64-bit `SHLegacyValue`, `sh_mirror.h:95-101`).

## Background

### Where the encode runs

`emit_shv_encode_for_slot_or_slow` (x86-64 `JitEmitter-internal.h:662-675`,
arm64 `:765-778`) calls the encoder under `HERMESVM_BOXED_DOUBLES` and
emits nothing otherwise. It has exactly two callers per backend, both at
the top of their tier, before any guard:

- PutByVal JSArray tier, `emitPutByValFastArrayTier`: x86-64
  `JitEmitter-property.cpp:91-92`, arm64 `:94-95`.
- PutById tier, `emitPutByIdInlineTier`: x86-64 `:2373-2374`, arm64
  `:2552-2553`.

The encoded value lives in `temp2` from there to the store. The encode
runs first on purpose: when most declines were box declines, rejecting
them before the guards halved the Box2D regression (doc/JIT.md, "The
encode runs first"; x86-64 `JitEmitter-internal.h:651-655`). Both tiers
then store through `emitSafeStoreOrSlow` (x86-64 `:202`, `:2410`; arm64
`:249`, `:2591`), its only callers.

### emitSafeStoreOrSlow

x86-64 `JitEmitter-internal.cpp:496-613`, arm64 `:488-631`, compiled
only under `HERMES_JIT_INLINE_SAFE_STORE`, which requires Hades
(`include/hermes/VM/JIT/Config.h:81-85`). Its parameters are `loc` (slot
address), `shv` (encoded value), `value` (original HermesValue), `t1`,
`t2`, `slowLab`. In order (x86-64 / arm64 lines):

1. `t1 = loc & ~(kSegmentUnitSize - 1)` (`:526-527` / `:536`).
2. `t1 == runtimeHadesYGStart` -> `youngTargetLab` (`:532-533` /
   `:541-543`).
3. Old-generation marking barriers on -> `slowLab` (`:539-542` /
   `:549-551`).
4. Compaction in progress -> `slowLab` (`:550-554` / `:557-563`).
5. Segment not exactly one unit long -> `slowLab` (`:570-571` /
   `:579-581`).
6. Store `shv` (`:574` / `:587`).
7. Tag of the ORIGINAL `value` not a pointer -> `doneLab` (`:583-585` /
   `:596-598`).
8. `value`'s cell outside the young generation -> `doneLab` (`:590-593`
   / `:603-608`).
9. Dirty `loc`'s card, `jmp doneLab` (`:599-606` / `:618-624`).
10. `youngTargetLab`: store `shv`, no barrier (`:608-609` / `:626-627`).
11. `doneLab`.

Steps 7-8 test `value`, not `shv`, and the code says a BoxedDouble "can
never be here, because the encode declined it" (x86-64 `:577-582`,
arm64 `:590-595`; also x86-64 `JitEmitter.h:1646-1650` and doc/JIT.md).
The header also says the function "emits no call" (x86-64
`JitEmitter.h:1709-1710`, arm64 `:1647-1648`).

### Inline young-generation allocation

`allocInYoung(kind, sz, out, temp1, temp2, slowPathLab)` (x86-64
`JitEmitter-alloc.cpp:138-149`, arm64 `:97-108`):

- `bumpAllocAndUnpoison`: loads `runtimeHadesYGLevel` and
  `runtimeHadesYGEnd` (`RuntimeOffsets.h:171-175`), computes the new
  level, jumps to `slowPathLab` if it passes the end (x86-64 `:38-39`,
  arm64 `:28-34`), and only then stores the new level (x86-64 `:85`,
  arm64 `:54`). A failed allocation publishes nothing.
- ASan builds only: pushes the eight x86-64 GP temps (arm64: x0-x15)
  around a call to `__asan_unpoison_memory_region` (x86-64 `:41-82`,
  arm64 `:36-51`). Precondition: no live value in a vector temp or
  `xScratch`; `out`, `temp1`, `temp2` not `xScratch` (x86-64
  `JitEmitter.h:2176-2181, 2207-2210`; arm64 has the same behavior but
  its declaration, `JitEmitter.h:2096-2128`, does not document it).
- `initGCCell` (x86-64 `:99-136`, arm64 `:65-95`) writes the debug magic
  and `debugAllocationId` (Debug only, from the same counter
  `nextObjectID()` uses, `GCBase.cpp:1078-1080`) and `KindAndSize`.
- No call that can collect: the bump is not a GC safepoint.

### BoxedDouble

`include/hermes/VM/BoxedDouble.h:19-48`: a `GCCell` with one `double
value_`, `CellKind::BoxedDoubleKind`, no finalizer, no pointer fields
(`lib/VM/BoxedDouble.cpp:15-20`). `RuntimeOffsets::boxedDoubleValue` is
`offsetof(BoxedDouble, value_)` (`RuntimeOffsets.h:73`); the size is
`cellSize<BoxedDouble>()`, as its VTable uses. Its SmallHermesValue is
the compressed pointer with tag `BoxedDouble = 4` in the low three bits
(`SmallHermesValue.h:256-316`; `sh_small_hermes_value.h:22-27`). The
emitted compressed-pointer encode is `emit_sh_cp_encode_non_null`
(x86-64 `JitEmitter-internal.h:410-416`, arm64 `:515-521`): a subtract
of the heap base under `HERMESVM_COMPRESSED_POINTERS` (HV32), nothing
under BOXED. The GetByVal tier already decodes a box inline
(`Emit_sh_shv_decode`, x86-64 `JitEmitter-internal.cpp:100-147`, arm64
`:85-114`).

## Design

### Store sequence under HERMESVM_BOXED_DOUBLES

The encode moves from the top of each tier into `emitSafeStoreOrSlow`,
after its declining checks and before its store. Once a box no longer
declines, an early encode has little left to reject cheaply: the
encoder's other declines are the symbol arm's raw-HV32 case (x86-64
`JitEmitter-internal.cpp:255-262`, arm64 `:244-253`) and the Handle-San
arm (not built with inline stores locally). The cost of the move is that
a box store declined for old-generation marking now runs the tier's
guards before declining; see Costs.

The encoder is a free function (x86-64 `JitEmitter-internal.cpp:151`,
arm64 `:119`) and cannot call the member `allocInYoung`. It gains a
`boxLab` parameter: a double that needs a box jumps there instead of to
`slowLab`. `emitSafeStoreOrSlow` binds `boxLab` and emits the box arm.
`emit_shv_encode_for_slot_or_slow` loses both callers and is deleted.

New sequence (BOXED_DOUBLES builds only):

```
  t1 = loc & segMask
  if t1 == YGStart: goto join          ; young target: no checks
  if ogMarkingBarriers: goto slow      ; steps 3-5 unchanged
  if compactee active:  goto slow
  if segment size != 1: goto slow
join:                                  ; t1 = segment start, live
  t2 = encode(value, temp t3)          ; inline encodings as today;
                                       ; raw-HV32 symbol -> slow;
                                       ; needs a box -> boxLab
  store t2 -> [loc]
  if t1 == YGStart: goto done          ; young target: no card
  steps 7-8 on `value` as today        ; pointer into the young gen?
  goto dirtyCard / done
boxLab:
  t2 = allocInYoung(BoxedDoubleKind, cellSize<BoxedDouble>(),
                    temps t1, t3; full -> slowFull)
  [t2 + boxedDoubleValue] = value      ; the double's bits
  t2 = cp_encode(t2) | Tag::BoxedDouble
  store t2 -> [loc]
  t1 = loc & segMask                   ; t1 was an allocation temp
  if t1 == YGStart: goto done
dirtyCard:                             ; a box is always young
  card(loc) = dirty
done:                                  ; falls into the tier's jmp contLab
...
slowFull:                              ; out of line: counter, then slow
```

`done` falls through into the tier's `jmp contLab` (x86-64
`JitEmitter-property.cpp:202` then `:476-477`, `:2410` then
`:2490-2491`), so `slowFull` is emitted out of line, after an
unconditional jump. Without `-Xjit-emit-counters` it is `slowLab`
itself.

Every declining check precedes the allocation, so a box is never
allocated and abandoned, and nothing is stored before a decline: the
marking barrier needs the old slot value (x86-64
`JitEmitter.h:1664-1667`). After the join, the declines are the
raw-HV32 symbol, the Handle-San arm and, on the box arm, a full young
generation; none happens after an allocation or a store. A failed bump
jumps before the level is stored (x86-64 `JitEmitter-alloc.cpp:38-39,
85`), so it publishes nothing.

The state the checks read cannot change before the store. The marking
flag and the compactee start are written only by the mutator, inside the
collector (`HadesGC.cpp:1601, 1764, 1808, 1910`), and nothing between
the checks and the store can collect: the bump allocation is not a
safepoint, and in ASan builds its only call is
`__asan_unpoison_memory_region`. The young-generation bound the inline
bump compares against is the one the C++ allocator uses
(`effectiveEnd_`, `AlignedHeapSegment.h:872`; `RuntimeOffsets.h:174-175`).

The box is in the young generation by construction, so the box arm
dirties an old target's card unconditionally; a young target needs no
barrier for any value (step 10 today).

### Join-point contract (both backends)

At `join`: live are `loc`, `value` and `t1` (the segment start). `t2`
and a new temporary `t3` are free, and none of the three is `xScratch`.
On the box arm `t1` is used as an allocation temporary and recomputed
from `loc` after the store. Nothing is live in a vector register or
`xScratch`: the PutByVal key and its scratch (`key`, `keyTmp`) are dead
after the index guard (x86-64 `JitEmitter-property.cpp:135-142`). That
satisfies `allocInYoung`'s ASan precondition. `loc` and `value` survive
the ASan unpoison call: they are GP temps, which it saves, or
callee-saved globals, which the ABI preserves.

Under BOXED_DOUBLES `emitSafeStoreOrSlow` takes `value`, `t1`, `t2`,
`t3` and the tier's box counter (a `JitCounter`, below) and computes the
encoded value itself; the `shv` parameter goes away. Each tier allocates `t3` in its prologue beside the temps it
already picks, which emits no code (x86-64 `JitEmitter-property.cpp:54-77`,
arm64 `:54-79`). The budget allows it without spilling: with `t3` the
x86-64 PutByVal tier uses at most 7 of its 8 GP temps and the PutById
tier at most 6; arm64 has 16 (`JitEmitter.h:200-216` x86-64, `:201-224`
arm64). `target` is dead at the join but must not be used as a
temporary: it may be a callee-saved global holding its frame register.

On arm64, `emitSafeStoreOrSlow` loads runtime words through `xScratch`
(`JitEmitter-internal.cpp:541-622`); none is held across the allocation.

### HV64

Under HV64 neither tier encodes, `t3` is not allocated, and
`emitSafeStoreOrSlow` keeps its current two-arm sequence. The HV64
emitted code of both tiers must not change (criterion 2).

### Decline accounting

A young-generation-full fallback reaches the same helper as any other
decline and is counted the same way. `declineCount` belongs to the
function version, not the site: every call to a recording helper from
any site of the body increments it (`lib/VM/JIT/JitHandlers.cpp:360,
481, 497`). A demoted ByVal site calls the plain
`_sh_ljs_put_by_val_*_rjs` instead (`demoteSite`, `JitCompiler.cpp:369-379`;
retirement flips every still-recording site the same way, `:390`), so
its declines, fallbacks included, record nothing and count toward no
threshold; the emitted `NumStoreBoxFallbacks` still counts them. A
threshold crossing runs `considerRecompile`, which recompiles if any
site of the body made progress and moves every recording site toward
demotion (`JitCompiler.cpp:430-518`). So a fallback can contribute to a
crossing that recompiles for another site's progress, or that demotes a
site. Both are what any other decline does, and neither removes an
inline tier: demotion only swaps a ByVal site's helper pointer
(`demoteSite`, `:369-379`), and the inline tier still runs first.

A ByVal site whose first decline is a fallback records its target's
shape then (`jsArraySeen` for a JSArray), as for any first decline.

Frequency: each fallback is a store that found the young generation
full and triggers the collection that empties it, so box fallbacks are
at most one per young-generation collection. Nothing bounds them by the
number of boxes: other allocations can fill the young generation
between box stores, and an external-memory charge can lower its
effective end to the current level (`HadesGC.cpp:2042-2054`;
`AlignedHeapSegment.h:872, 894`), so the next box store falls back at
once. In a loop that allocates nothing but boxes, a fill holds at most
`ygSizeFactor_` (0.25 to 1.0) times the segment's allocation region
(`HadesGC.cpp:2787-2789, 2943-2953`), up to about 262K boxes at 16
bytes (Release, to be confirmed by `cellSize<BoxedDouble>()`). Either
way a fallback costs what today's decline of the same store costs, plus
the bump that failed.

The larger effect goes the other way. Under HV32, box declines were most
of the declines on numeric code, and so most of the threshold
crossings. With them gone, recompile checks come later or not at all.
Benchmarks report `NumRecompileChecks` and `NumRecompiles` before and
after.

### Counters

Three counters join `JIT_COUNTERS` (`include/hermes/VM/JIT/JitCounters.h:17-46`),
emitted only under `-Xjit-emit-counters` like the existing ones:

- `NumPutByValBoxes`, `NumPutByIdBoxes`: a box allocated and stored
  inline by that tier. Incremented after the box arm's store. Separate
  counters, so a test can tell which tier's box arm ran.
- `NumStoreBoxFallbacks`: the box arm found the young generation full.
  Incremented at `slowFull`.

`emitIncrementCounter` clobbers `xScratch` and EFLAGS on x86-64 (x86-64
`JitEmitter.cpp:587-602`) and saves and restores x0/x1 around its
update on arm64 (arm64 `JitEmitter.cpp:656-670`). Neither point holds a
live value in `xScratch` or the flags. Counter instrumentation changes
the emitted code, so counter runs and timing runs are separate.

### Costs

- Every store in BOXED_DOUBLES builds gains one compare and branch (the
  young-target test after the store, where today's young arm stores and
  jumps to `done`).
- An inline (non-box) store of a young pointer into an old target takes
  an extra unconditional `jmp dirtyCard` (`b` on arm64) to reach the card
  block, which the box arm shares; today the card block follows directly.
- The box arm is emitted at every store site of both tiers: the bump,
  the cell header, the double store, the encode, the store and the card
  path. The plan measures the code size per site and per benchmark.
- A box store declined for marking or compaction (36-39% of
  NavierStokes' box stores, dz 01a185d0-5d43) now runs the tier's guards
  before declining, where today the encode declined it first. doc/JIT.md
  (around line 720) measured this ordering effect on Box2D when the
  encode was moved to the front. The benchmarks show whether it matters
  now that most of those stores no longer decline at all.

### Comments and docs

Update what this makes false or stale:

- The "BoxedDouble can never be here" comment (x86-64
  `JitEmitter-internal.cpp:577-582`, arm64 `:590-595`) and the header
  docs of `emitSafeStoreOrSlow` (x86-64 `JitEmitter.h:1633-1650,
  1696-1699, 1709-1710`, and the arm64 copies), including "emits no
  call", which stays true except in ASan builds, where the box
  allocation calls the unpoison function with the GP temps saved.
- The encode-first comments: x86-64 `JitEmitter-internal.h:651-655`,
  x86-64 `JitEmitter-property.cpp:80-90, 2362-2372`, arm64
  `JitEmitter-property.cpp:84-93, 116-121` and its PutById copy, arm64
  `JitEmitter-internal.cpp:497-501`.
- x86-64 `JitEmitter-internal.h:628` ("one caller is
  Emitter::emitSafeStoreOrSlow"), false today and true afterwards: keep
  it.
- doc/JIT.md: "The encode runs first", the safe-store predicate section
  (around lines 630-668) and its "Tests" paragraph.
- Comments in `test/jit/inline-store-shv-shapes.js` (the header and
  the comment above the `boxed` loop, which says every one of those
  stores declines to the helper) and
  `test/jit/x86-64/putbyval-inline-emitted.js:52-57`.
- Document `allocInYoung`'s ASan precondition on the arm64 declaration
  (`JitEmitter.h:2096-2128`).

## Testing

Arch-independent tests run on every tree; per-mode expectations use the
`%hv-mode` substitution (`test/lit.cfg:98-122`). Counter checks use
`{{[1-9][0-9]*}}` for "nonzero" and exact values where derived. Per
DESIGN-PRINCIPLES.md 6, the failure each test catches is the mutation
named with it, checked during implementation: with the counters present,
remove the box arm (jump to `slowLab` instead) and the test must fail.

Values that box in every BOXED_DOUBLES mode: `1 + (2k + 1) * 2^-52` for
integer `0 <= k < 2^51`. They are exact doubles whose lowest mantissa bit is set, so
the low 35 bits (HV32) and the low 3 bits (BOXED) are nonzero
(`SmallHermesValue.h:323-337`), and distinct for distinct `k`. `0.1`
(`0x3FB999999999999A`) also boxes in both modes; `2^28 - 1`
(`0x41AFFFFFFE000000`) boxes under HV32 only.

Setup shared by the counting tests. `gc()` (defined when HermesInternal
is enabled, `GlobalObject.cpp:948-951`) runs `HadesGC::collect`: it
waits for any running collection, runs a young collection with a forced
old one, waits for it, and runs a second young collection that finishes
any pending compaction (`HadesGC.cpp:1469-1481`). Afterwards the young
generation is empty and the young collection has cleared the old
segments' cards (`HadesGC.cpp:3105-3119`). That last young collection
may start a new old-generation collection (`HadesGC.cpp:1477-1479`,
trigger at `:2836-2844`: `allocatedRatio >= ogThreshold_`), which would
turn marking on and prepare a compactee (`:1598`); `collect` does not
wait for it. The tests rule that out for their two `gc()` calls, given
two conditions on the test; the argument does not extend to an
arbitrary number of collections.

Write `B_n` for the old generation's allocated plus external bytes when
the sweep of the n-th completed old-generation collection finishes.

- The trigger is `(allocatedBytes() + externalBytes()) /
  targetSizeBytes() >= ogThreshold_` (`HadesGC.cpp:2836-2844`), checked
  only with no collection and no compactee active (`:2822`).
- The target is an average with weight 0.5 (`HadesGC.cpp:3358`,
  `ExponentialMovingAverage.h:36`) whose sample at each sweep is `B_n /
  occupancyTarget` = `2 B_n` (0.5, `GCConfig.h:152`;
  `HadesGC.cpp:1229-1238`), so `T_n = T_(n-1) / 2 + B_n`. It starts at
  `T_0` = `InitHeapSize` minus one segment's allocation region, at least 28 MiB with the default
  32 MiB (`GCConfig.h:146`, `HadesGC.cpp:1299-1301`). With `B_1, B_2 <=
  7 MiB`: `T_1 >= 14 MiB + B_1` and `T_2 >= 7 MiB + B_2 >= 2 B_2`, and
  likewise `T_1 >= 2 B_1`.
- When `gc()` makes its final check, no JavaScript has run since the
  sweep and the young generation is empty. Evacuating the compactee
  allocates each cell at its own size (`HadesGC.cpp:505-514`) and
  `finalizeCompactee` then subtracts the compactee's bytes
  (`:1771-1798`); the first young collection already moved the young
  generation's external charges to the old generation (`:2773-2774,
  2935-2938`), so the second adds none. The numerator is at most `B_n`,
  and for the tests' two calls the ratio is at most 0.5.
- `ogThreshold_` starts at 0.75 (`HadesGC.h:1256`, weight 0.5) and is
  updated only when an old-generation collection completes, to `r / (r +
  1)` for the clamped mark rate `r` (`HadesGC.cpp:1828-1854`). A forced
  collection's marking runs while `waitForCollectionToFinish` drives
  `incrementalCollect` and no JavaScript runs (`HadesGC.cpp:1525`), in
  concurrent and incremental Hades alike, so `r` clamps to 20 and the
  update value is about 0.95. If every completed collection was forced,
  the threshold is at least 0.75 > 0.5, and the final young collection
  of `gc()` starts nothing: marking and compaction are off when it
  returns.

The two conditions that make every completed collection a forced one:

1. No natural collection before the first `gc()`. Until the first
   completion the threshold is 0.75 and the target is `T_0 >= 28 MiB`, so
   a natural collection needs allocated plus external bytes of 21 MiB.
   The tests stay under 7 MiB (which also gives `B_1, B_2 <= 7 MiB`), create no
   object that charges external memory (no ArrayBuffer, typed array or
   external string), and pass no GC flags (`-gc-init-heap`,
   `-gc-occupancy-target` and the like change these numbers).
2. No young collection after the first `gc()` until the measured stores
   are done, so none can start a natural collection in between: the
   allocation between and after the `gc()` calls is far below the
   smallest young generation (0.25 of the segment's allocation region,
   about 1 MiB, `HadesGC.cpp:2943-2953`). External charges can also
   shrink the young generation (`HadesGC.cpp:2042-2054`); the tests make
   none.

The plan checks both conditions on each tree once, with
`-gc-print-stats` run separately from the test's own RUN lines.

Each counting test therefore (a) calls the
store functions once on a throwaway target so that they are compiled and
their caches warm before the measured target exists, (b) calls `gc()`,
(c) allocates the measured target, and promotes it with a second `gc()`
where it must be old, and (d) runs a measured loop whose allocation
total (boxes included) is far below the smallest young generation (0.25
of the segment's allocation region, about 1 MiB), so no collection
starts inside it. With no collection inside the loop, marking and
compaction stay off and the target keeps its generation, so every
measured store passes the predicate and only the box arm decides.

1. **PutByVal boxes inline** (new, `test/jit/store-boxed-double-byval.js`,
   `-Xjit=force`). In `main()`, with the setup above: a store site in a
   separate function passed as a parameter, and a measured array
   prefilled with N small integers (so every index is inside the range
   guard and no slot is a hole, x86-64 `JitEmitter-property.cpp:144-148,
   182-188`). The loop stores N boxing values, then every value is read
   back and compared. Two RUN lines: the array young (no second `gc()`),
   and old. Expected: values (all modes); `NumPutByValBoxes: N` under
   HV32 and BOXED, `NumPutByValBoxes: 0` under HV64, with N = 1000 (24
   KB of boxes at the Debug size). The store function's warm-up call
   stores inline-encodable values only, so it adds no boxes.
2. **PutById boxes inline** (new, `test/jit/store-boxed-double-byid.js`,
   threshold mode, because the PutById tier is emitted only when the
   site's write cache is warm at compile time, x86-64
   `JitEmitter-property.cpp:2440-2493`). The warm-up of step (a) calls
   the store function with inline-encodable values until it is compiled;
   the effective threshold depends on the function's loop depth (x86-64
   `include/hermes/VM/JIT/x86-64/JIT.h:284`, arm64 `arm64/JIT.h:276`),
   so the warm-up count is derived from it in the plan, and the plan
   confirms compilation before the measured loop, for example with
   `-Xdump-jitcode=2` (`RuntimeFlags.h:269`). The measured loop then stores N
   boxing values into an existing property of the measured object, every
   call compiled. Expected: values; `NumPutByIdBoxes: N` under HV32 and
   BOXED, `0` under HV64; same young and old RUN lines.
3. **Old target gets its card** (new, `test/jit/store-boxed-double-card.js`,
   one case per tier, each in its own RUN line so one case cannot dirty
   the other's card). With the setup above, the target promoted by the
   second `gc()`: its cards are clean, nothing else is written in the
   case, and exactly one box is stored into it. A young collection is
   then forced by allocating more than the young generation holds,
   followed by a read-back of the slot. Expected: the value; the tier's
   box counter is 1 under HV32 and BOXED. Mutation, per tier: drop the
   box arm's card dirty; the case must fail under `HERMES_SLOW_DEBUG`,
   where `HadesGC::verifyCardTable()` (`HadesGC.cpp:2684`) finds an
   old-to-young pointer on a clean card before the young collection, or
   in the read-back. Not masked: the card was cleared by `gc()` and no
   other store in the case writes an old object, so only the box arm can
   dirty it (a card covers 512 bytes, `AlignedHeapSegment.h:97`).
   `test/jit/inline-store-shv-shapes.js` keeps its role as the all-tags
   behavioral test, with its comments updated.
4. **Young generation full** (new, `test/jit/store-boxed-double-ygfull.js`,
   `-Xjit=force`). A loop overwrites slot 0 of a small array with a
   fresh boxing value and reads it back each iteration, comparing it to
   the value just stored (a double comparison, no allocation). The loop
   allocates nothing else, and runs at least `2 * 4 MiB / 16` = 524,288
   iterations: two fills of the largest young generation at the Release
   box size. Expected: no mismatch; `NumStoreBoxFallbacks` and
   `NumPutByValBoxes` nonzero under HV32 and BOXED; both `0` under HV64.
   The first fallback is deterministic: the array is young until the
   first collection, so every store until then takes the young arm, and
   the box allocation is the only allocation that can find the young
   generation full.
5. **Emitted code.** The HV32 and BOXED pins of
   `test/jit/x86-64/put{byval,byid}-inline-emitted.js` and
   `test/jit/put{byval,byid}-inline-emitted-arm64.js` are updated to the
   new sequence: the encode after the checks, the box arm, and its card.
   The directives active under HV64 are not changed: a directive shared
   by all modes that the boxed layout breaks moves to an HV64-only
   prefix with its text and order intact.
6. **Existing behavioral tests** `test/jit/putbyval-inline.js` and
   `test/jit/putbyid-inline.js` pass unchanged in every mode.
7. **A duo site runs the box arm** (new, added after review,
   `test/jit/store-boxed-double-duo.js`, `-Xjit=force
   -Xjit-recompile-threshold=4`). `storeDuo(t, i, v)` declines four times
   on a 4-element Float64Array, so its version 2 puts the typed-array
   tier ahead of the JSArray tier. The `-Xdump-jitcode=3` output must
   show version 2 compiled, with both tiers and the box arm, before a
   printed marker, and nothing compiled after it. The Float64Array is
   allocated before the first `gc()`: its 32-byte external charge
   shrinks the young generation only until that collection moves it to
   the old generation, a deliberate exception to condition 1. With the
   setup above, K = 1000 boxing stores go into a young JSArray prefilled
   with integers, and are read back. Expected: values;
   `NumPutByValBoxes: 1000` and `NumStoreBoxFallbacks: 0` under HV32 and
   BOXED, 0 boxes under HV64. Mutation A fails it.
8. **PutById young generation full** (new, added after review,
   `test/jit/store-boxed-double-ygfull-byid.js`). The PutById
   counterpart of test 4: 524,288 fresh boxing stores to `o.p`, each
   read back, with `p` in a direct slot and, in a second run, in an
   out-of-line slot, whose propStorage cell the fallback's collection
   also moves. A cold PutById body holds only the helper, so the
   startup is explicit: the store function is warmed with integers
   until its body with the tier is installed (threshold mode: the fifth
   call; `-Xjit=force`: version 2, after four helper declines with
   `-Xjit-recompile-threshold=4`). The dump shows that body before a
   marker and nothing compiled after it; then `gc()`, and a fresh
   measured object of the same class. Both modes run with and without
   counters. Expected: no mismatch; `NumPutByIdBoxes` and
   `NumStoreBoxFallbacks` nonzero under HV32 and BOXED, both 0 under
   HV64. Fails if the PutById tier's fallback skips the helper (all
   eight runs: mismatches) or is not counted (the counter runs).

Runs: the full lit suite on `cmake-build-x86jit` (HV64),
`cmake-build-x86jit-hv32` and `cmake-build-x86jit-boxed` (ASan Debug);
the JIT suite on `cmake-build-arm64{,-hv32,-boxed}` under qemu
(aarch64/README.md:100), plus `aarch64/qemu-sanity.sh` on each. A
missing card fails loudly only under `HERMES_SLOW_DEBUG`, so Release
passing is not evidence (doc/JIT.md, "Tests").

Not tested: the Handle-San arm with inline stores (no local tree builds
`HERMESVM_SANITIZE_HANDLES` with Hades and BOXED_DOUBLES); arm64 under
ASan (the arm64 trees are built without it).

## Benchmarks

Octane without RegExp on x86-64 Release, `HERMESVM_ALLOW_JIT=2`
(`cmake-build-jit2`, `cmake-build-jit2-hv32`), before and after, from
bytecode compiled by each build's own `hermesc -O`, `taskset -c 2`, 5
runs with the configuration order rotated, medians. Report:

- every benchmark under both modes, and the HV32 JIT/interp ratios
  against the 2026-10-28 table in dz 01a13c62;
- perf shares of the generic PutByVal path on NavierStokes and Crypto;
- from separate counter runs: `NumPutByValBoxes`, `NumPutByIdBoxes`,
  `NumStoreBoxFallbacks`, `NumRecompileChecks` and `NumRecompiles`, per
  benchmark, before and after;
- emitted code size per benchmark under HV32, before and after.

## Delivered (2026-10-29)

This change (x86-64, arm64 and doc/JIT.md) is on branch `x86-jit` (base
`630ff84d1`). All four success criteria are met. Under HV32,
NavierStokes is 9.6% faster, Crypto 21.8% and Box2D 17.1%. Two HV32
benchmarks that box nothing lost ground beyond noise: zlib by 2.7% and
DeltaBlue by 1.0%. See Benchmarks.

### Differences from the spec

- The HV64 pins changed after all. Two directives shared by every mode
  cannot hold for the boxed-mode layout: the young label now comes
  before the store, and the card block falls through into `done`. They
  were re-prefixed `SPEC-HV64` (x86-64 putbyid: `[[YOUNG]]:`; arm64
  putbyid: `b [[DONE]]` and `[[YOUNG]]:`), and the object-check lines of
  the HV32 and BOXED prefixes became label definitions. Every directive
  active under HV64 is verbatim and in its old order, and HV64 code
  identity was checked separately (Matrix).
- Layout: `slowFull` sits after the inline path's unconditional `jmp
  dirtyCard` and before `boxLab`. The box arm falls into the card
  block, which falls into `done`.
- `t3` is allocated after each tier's existing temporaries, so the
  other temporaries get the same registers as in the default mode.
- Some stale comments were fixed that the spec's list does not name:
  the GetByVal tiers' mention of the deleted wrapper (both backends'
  `JitEmitter-property.cpp`), the arm64 PutById xScratch comment, the
  "encode comes first" headers of the four pin files, the
  `inline-store-shv-shapes.js` bullet and the build-matrix paragraph of
  doc/JIT.md.
- Tests beyond the spec's list: the card test's out-of-line-slot case,
  and `store-boxed-double-values.js` (edge values, both tiers, a site
  that stores two values). The counting tests also run with
  `-Xdump-jitcode=2`, to check that nothing compiles inside the measured
  region. An external review then found two gaps, closed by tests 7 and
  8 in Testing: `-values.js` checks only that a duo site's version 2
  emits the box arm, not that it runs (`-duo.js`), and no test drove the
  PutById tier's young-generation-full fallback (`-ygfull-byid.js`).
- `store-boxed-double-ygfull.js` does not catch mutation B (the box
  arm's card dirty removed). After each young collection, the first
  store into the old array is the fallback's helper store. Its runtime
  barrier dirties the card, so every later inline box store lands on a
  card that is already dirty. `store-boxed-double-card.js` catches B on
  both backends.
- The x86-64 `JitEmitter.h` pseudocode marks the young-segment exit
  `// young: no card`, not `// boxed doubles`. Under HV64 a young
  target also skips the card test.

### Matrix

| tree | what ran | result |
|---|---|---|
| cmake-build-x86jit (ASan Debug, HV64) | check-hermes | 4598 passed, 6 expected failures, 162 unsupported, 0 unexpected |
| cmake-build-x86jit-hv32 | check-hermes | 4599 / 6 / 162 / 0 |
| cmake-build-x86jit-boxed | check-hermes | 4599 / 6 / 162 / 0 |
| cmake-build-arm64, -hv32, -boxed (qemu) | qemu-sanity.sh, then `LIT_FILTER='jit/'` (Task 2) | sanity OK; 163 passed, 31 unsupported, 0 failed on each |
| all six trees, after tests 7 and 8 | `LIT_FILTER='jit/'` | x86-64: 188 passed, 8 unsupported, 0 failed on each; arm64 (qemu): 165 passed, 31 unsupported, 0 failed on each |
| x86-64 and arm64 hv32 and boxed | tests 7 and 8, with counters | duo: `NumPutByValBoxes: 1000`, 0 fallbacks; ygfull-byid, every run: 524284 inline boxes and 4 fallbacks (their sum is the 524,288 stores) |
| x86-64 and arm64, all six trees | gc-check.py: the byval/byid young and old runs and the card runs | all rc 0. Forced young/old collections, old-gen starts and compactions: 2/1/1/0 (young runs), 4/2/2/0 (old and card runs); peak allocated+external 216-260 KB; no natural collection in the young and old runs, 4-8 natural young collections at the end of the card runs |
| x86-64 ASan HV64, base vs head | `hv64-ident.sh --corpus`, same-length binary paths | 170 IDENTICAL (6 inputs, 164 JIT tests), 0 differences |
| arm64 HV64 (qemu), base vs head | `hv64-ident.sh --qemu` | 5 inputs IDENTICAL; Box2D DIFFERENT twice, by one extra body (below) |
| arm64 HV64, base vs head | Box2D with `-Xjit=force -Xjit-max-recompiles=0` | 337 bodies, 598406 lines, 0 differences |
| Release x86-64 HV64, base vs head | body sizes, recompilation off | 11 of 13 benchmarks: every common body the same size; 2 bodies vary run to run (below) |
| controls (Task 1) | base against itself, x86-64 `--corpus` and arm64 | IDENTICAL (one x86-64 control run differed on Box2D by one extra body; the rerun was 165 of 165) |
| sensitivity | `hv64-sens.py`, arm64 putbyval dump, base vs head | const, helper and stale REPORTED, moved ACCEPTED |
| sensitivity | x86-64 putbyval dump, base vs head | const, helper and stale REPORTED, moved ACCEPTED |
| sensitivity | x86-64 putbyid dump, base vs head | exit 2, "no helper whose address differs": none of its 3 helpers moved; const and helper REPORTED on the same dump against itself |

Mutations. Each was applied, the tree rebuilt and the tests run, then
the code was restored, rebuilt and rerun green:

| mutation | x86-64 | arm64 |
|---|---|---|
| A: box arm to `slowLab` | hv32 and boxed: byval, byid, card, ygfull fail on `NumPut*Boxes: 0`; both pin files fail. Later, hv32: duo (`NumPutByValBoxes: 0`) and ygfull-byid (`NumPutByIdBoxes: 0`) | hv32: the same 6 failures. Later: duo and ygfull-byid, the same |
| B: card dirty skipped (`je`/`b.eq` to unconditional) | card (byval, byid, byidwide each exit 134 in `verifyCardTable`), values (same assertion), both pins; ygfull passes | hv32 and boxed: the same |
| C: no pointer compression | hv32: all 7 fail (ASan SEGV, heap well-formedness); boxed: nothing fails, as expected | not run |
| D: half the double stored | hv32 and boxed: `mismatches` in byval, byid, ygfull, card `value false`, values, both pins | hv32: all 7 fail |
| E: fallback block jumps to `done` | hv32 ygfull: `mismatches 360103`. Later, a PutById-only variant (the tier's allocation-failure target is `done`, with and without counters): ygfull-byid fails in all 8 runs, `mismatches 420637` (direct) and `420638` (wide); ygfull and duo pass | not run |
| F: no fallback increment | hv32 ygfull: `NumStoreBoxFallbacks: 0`. Later: ygfull-byid's 4 counter runs, the same; its 4 counter-free runs pass | not run |

The number of timed Box2D iterations depends on speed, so whether
`runBox2D` gets compiled at the end does too. Under qemu, both arm64
runs compiled it on the base side only (Box2D scores 562 and 563,
against 537 and 545 for head). In the Task 2 run neither side compiled
it. The 407654 lines both sides share were identical. Under
`-Xjit=force` every body, `runBox2D` included, compiles on both sides,
and the dumps are identical.

The two Release body sizes that differ are `RSADoPrivate` in Crypto and
FunctionID 40 in Splay. Rerun with the same flags, the size of each
varies run to run on the same binary, base included. Feedback that
depends on timing is a likely cause, not verified.

### Benchmarks

Commands: the `bench.sh` and `summ.py` scripts of Task 4 in
doc/superpowers/plans/2026-10-29-jit-inline-boxed-double-store.md, run
as written there. Octane without RegExp, x86-64
Release, `HERMESVM_ALLOW_JIT=2`, from bytecode compiled by each build's
own `hermesc -O -emit-binary`, `taskset -c 2`, 5 reps with the
configuration order rotated per rep, medians. "noise" means the base and
head min-max ranges overlap. Every cell has 5 samples and no run failed.

HV64 JIT, base vs head:

| benchmark | base median [min-max] | head median [min-max] | head/base | verdict |
|---|---|---|---|---|
| richards | 7890 [7473-8059] | 7717 [7615-8179] | 0.978 | noise |
| deltablue | 7432 [7405-7445] | 7465 [7425-7478] | 1.004 | noise |
| crypto | 7219 [7210-7222] | 7212 [7174-7226] | 0.999 | noise |
| raytrace | 12776 [12580-12959] | 12802 [12395-12911] | 1.002 | noise |
| earley-boyer | 13207 [12823-13323] | 13230 [13106-13354] | 1.002 | noise |
| splay | 4673 [4636-4730] | 4681 [4619-4702] | 1.002 | noise |
| navier-stokes | 14500 [14485-14514] | 14514 [14485-14529] | 1.001 | noise |
| pdfjs | 20703 [20269-20825] | 20764 [20288-20784] | 1.003 | noise |
| mandreel | 4811 [4780-4832] | 4811 [4770-4825] | 1.000 | noise |
| gbemu | 20679 [19509-20864] | 20518 [20087-20679] | 0.992 | noise |
| box2d | 24039 [23762-24086] | 23857 [23668-24182] | 0.992 | noise |
| zlib | 6412 [6404-6426] | 6401 [6294-6438] | 0.998 | noise |
| typescript | 55102 [54770-55485] | 55365 [54747-55510] | 1.005 | noise |
| geomean | | | 0.998 | |

HV32 JIT, base vs head:

| benchmark | base median [min-max] | head median [min-max] | head/base | verdict |
|---|---|---|---|---|
| richards | 6008 [5857-6280] | 5874 [5617-6015] | 0.978 | noise |
| deltablue | 6010 [5997-6017] | 5951 [5918-5964] | 0.990 | slower |
| crypto | 5380 [5374-5405] | 6555 [6516-6555] | 1.218 | faster |
| raytrace | 10813 [10687-10919] | 10824 [10782-10919] | 1.001 | noise |
| earley-boyer | 12979 [12916-12992] | 12905 [12830-12968] | 0.994 | noise |
| splay | 6038 [6006-6179] | 6037 [6018-6130] | 1.000 | noise |
| navier-stokes | 7060 [6984-7081] | 7741 [7678-7764] | 1.096 | faster |
| pdfjs | 19990 [19911-20010] | 19951 [19737-19990] | 0.998 | noise |
| mandreel | 4206 [4134-4208] | 4197 [4197-4199] | 0.998 | noise |
| gbemu | 19312 [19067-19312] | 19545 [18947-19618] | 1.012 | noise |
| box2d | 14297 [14241-14339] | 16741 [16282-16911] | 1.171 | faster |
| zlib | 6151 [6133-6164] | 5984 [5979-5998] | 0.973 | slower |
| typescript | 50239 [49268-50517] | 51372 [51145-51434] | 1.023 | faster |
| geomean | | | 1.032 | |

HV32 JIT/interp, with the head interpreter as the denominator, beside
the 2026-10-28 ratios of dz 01a13c62 (built at 7357a443f). On zlib the
head interpreter is about 2.5% slower than base's (median 3920 against
4022 in the JIT-off runs below), so base's zlib ratio (1.57) is
inflated:

| benchmark | interp | base JIT | x | head JIT | x | 2026-10-28 x |
|---|---|---|---|---|---|---|
| richards | 2453 | 6008 | 2.45 | 5874 | 2.39 | 2.46 |
| deltablue | 2202 | 6010 | 2.73 | 5951 | 2.70 | 2.67 |
| crypto | 2904 | 5380 | 1.85 | 6555 | 2.26 | 1.85 |
| raytrace | 7128 | 10813 | 1.52 | 10824 | 1.52 | 1.51 |
| earley-boyer | 9224 | 12979 | 1.41 | 12905 | 1.40 | 1.39 |
| splay | 5825 | 6038 | 1.04 | 6037 | 1.04 | 1.03 |
| navier-stokes | 4573 | 7060 | 1.54 | 7741 | 1.69 | 1.52 |
| pdfjs | 14437 | 19990 | 1.38 | 19951 | 1.38 | 1.38 |
| mandreel | 2264 | 4206 | 1.86 | 4197 | 1.85 | 1.88 |
| gbemu | 14172 | 19312 | 1.36 | 19545 | 1.38 | 1.34 |
| box2d | 11274 | 14297 | 1.27 | 16741 | 1.48 | 1.25 |
| zlib | 3914 | 6151 | 1.57 | 5984 | 1.53 | 1.52 |
| typescript | 34396 | 50239 | 1.46 | 51372 | 1.49 | 1.45 |
| geomean | | | 1.59 | | 1.65 | 1.58 |

The two HV32 losses:

- zlib boxes nothing: all three box counters are 0. Its HV32 code grew
  5.1%. Three pairs of profiles were taken. In the first, the JIT-code
  share is about the same (52.4% base, 51.2% head), and the symbol that
  grew is `JSObject::putComputedWithReceiver_RJS`, from 0.98% to 3.11%
  of samples. A second pair gives 0.88% and 3.04%. A third pair, with
  call graphs, gives 0.84% and 3.25%, and in head nearly all of it
  (3.03 of 3.25 points) is under `Interpreter::interpretFunction`,
  which runs zlib's eval'd code. This change does not touch the
  interpreter: outside `lib/VM/JIT` it edits only
  `include/hermes/VM/JIT/JitCounters.h`. With the JIT off the head
  binary is also slower on zlib: 3909, 3969 and 3920 against base's
  4043, 4007 and 4022. So most of the loss seems to come with the
  binary rather than the emitted store code, but the cause is not
  isolated.
- DeltaBlue boxes nothing either. Its HV32 code grew 1.6%. Its profile
  has no store helper above 1.6% in either build (`_jit_put_by_id` 1.6%
  base, 1.4% head), and the JIT-code share is 85.1% against 83.7%. The
  1.0% loss is not explained.

Counters, from separate runs (`-Xjit -Xjit-emit-counters`, same
bytecode). Base has no box counters. Under HV64 all three box counters
are 0 on every benchmark.

| benchmark | HV32 PutByValBoxes | PutByIdBoxes | StoreBoxFallbacks | HV32 RecompileChecks base -> head | HV32 Recompiles | HV64 Recompiles |
|---|---|---|---|---|---|---|
| richards | 0 | 0 | 0 | 5218 -> 5120 | 2 -> 2 | 2 -> 2 |
| deltablue | 0 | 0 | 0 | 136650 -> 135869 | 42 -> 42 | 42 -> 42 |
| crypto | 165952812 | 0 | 644 | 1972 -> 2346 | 26 -> 26 | 26 -> 26 |
| raytrace | 0 | 9125653 | 26 | 1017693 -> 885785 | 11 -> 11 | 11 -> 11 |
| earley-boyer | 0 | 0 | 0 | 1381365 -> 1396764 | 5 -> 5 | 5 -> 5 |
| splay | 0 | 688 | 0 | 158472 -> 161216 | 14 -> 14 | 14 -> 14 |
| navier-stokes | 55660531 | 0 | 220 | 96 -> 89 | 0 -> 0 | 0 -> 0 |
| pdfjs | 28132 | 77157 | 0 | 2977 -> 1923 | 82 -> 81 | 81 -> 81 |
| mandreel | 0 | 3932 | 0 | 29382 -> 29321 | 127 -> 127 | 127 -> 127 |
| gbemu | 0 | 244 | 0 | 315627 -> 315625 | 379 -> 379 | 379 -> 379 |
| box2d | 411322 | 17692772 | 71 | 277911 -> 42674 | 89 -> 86 | 88 -> 88 |
| zlib | 0 | 0 | 0 | 86 -> 86 | 29 -> 29 | 29 -> 29 |
| typescript | 0 | 9921193 | 38 | 561022 -> 383700 | 358 -> 357 | 356 -> 356 |

Code size: the sum of each compiled body's own bytes, with the number of
bodies in parentheses. "First" runs use `-Xjit-max-recompiles=0`, so both
sides compile first versions only; "all" runs allow recompilation.

| benchmark | HV32 first: base | head | head/base | HV32 all: base | head | head/base |
|---|---|---|---|---|---|---|
| richards | 105189 (32) | 108881 (32) | 1.035 | 117106 (34) | 121338 (34) | 1.036 |
| deltablue | 196975 (68) | 200126 (68) | 1.016 | 385725 (110) | 391846 (110) | 1.016 |
| crypto | 353815 (56) | 362775 (56) | 1.025 | 568714 (82) | 596018 (83) | 1.048 |
| raytrace | 271262 (42) | 273693 (42) | 1.009 | 527270 (53) | 530691 (53) | 1.006 |
| earley-boyer | 417107 (66) | 426840 (66) | 1.023 | 497706 (71) | 510623 (71) | 1.026 |
| splay | 60448 (16) | 62698 (16) | 1.037 | 151121 (30) | 157331 (30) | 1.041 |
| navier-stokes | 74099 (11) | 77552 (11) | 1.047 | 74099 (11) | 77552 (11) | 1.047 |
| pdfjs | 1882901 (153) | 1897280 (153) | 1.008 | 3572824 (235) | 3574297 (234) | 1.000 |
| mandreel | 5810897 (111) | 6074117 (111) | 1.045 | 15103464 (238) | 15768761 (238) | 1.044 |
| gbemu | 1338052 (243) | 1415636 (243) | 1.058 | 3715120 (623) | 3932049 (623) | 1.058 |
| box2d | 1946477 (229) | 1992322 (229) | 1.024 | 3903030 (318) | 3801570 (317) | 0.974 |
| zlib | 1301002 (27) | 1367909 (27) | 1.051 | 3657034 (56) | 3845874 (56) | 1.052 |
| typescript | 6726266 (557) | 6840270 (557) | 1.017 | 17463945 (915) | 17746945 (914) | 1.016 |

Under HV64, the first-version sums are equal on every benchmark except
Crypto (264253 against 264247) and Splay (44131 against 44109). Those are
the two bodies whose size varies from run to run (Matrix). With
recompilation, the sums differ only on Crypto (416160 against 416100)
and Box2D (2701254 in 345 bodies against 2692829 in 341).

Per site, the bodies of the emitted-code tests' inputs, HV32 (HV64 the
same in base and head):

| body | base | head | difference | HV64 |
|---|---|---|---|---|
| `store` (one PutByVal JSArray site) | 764 | 855 | +91 | 655 |
| `setIndirect` (one PutById site) | 621 | 711 | +90 | 521 |

Profiles (`perf record -F 2000`, one run each, `-Xjit`, HV32). These are
shares of samples. A symbol's share sums all of its rows, and "GC" sums
every symbol whose name contains `HadesGC`:

| bench | build | JIT code | putByValWithReceiver_RJS | ArrayImpl::_setOwnIndexedImpl | ArrayImpl::_haveOwnIndexedImpl | GC |
|---|---|---|---|---|---|---|
| NavierStokes | base | 46.4 | 19.1 | 14.0 | 2.1 | 12.7 |
| NavierStokes | head | 63.5 | 9.6 | 6.9 | 0.9 | 13.8 |
| Crypto | base | 69.9 | 15.0 | 8.9 | 1.7 | 0.3 |
| Crypto | head | 95.1 | 0.1 | 0.1 | 0.0 | 0.4 |
| Box2D | base | 60.5 | 0.1 | 0.2 | 0.1 | 1.3 |
| Box2D | head | 72.9 | 0.1 | 0.1 | 0.0 | 1.4 |

The generic PutByVal path is these three symbols plus
`_sh_ljs_put_by_val_*` (0.6% base, 0.2% head on NavierStokes; 0.6% and
0.0% on Crypto). It falls from 35.8% to 17.6% of NavierStokes' samples
and from 26.2% to 0.2% of Crypto's. On Box2D, `considerRecompile` falls
from 8.1% to 0.4% and `_jit_put_by_id` from 6.3% to 0.9%.

### Helper calls

The dz 01a185d0-5d43 instrumentation, with the ValueBox total added, on
`cmake-build-jit2-hv32` at head. Command: `taskset -c 2 hermes -Xjit
-Xjit-emit-counters -gc-print-stats <bench>.hbc`, from the head
bytecode. The patch was reverted afterwards and the tree rebuilt.
"Before" is the 2026-10-29 log entry. Its ValueBox breakdown is the sum
of the per-site lines (every site with at least 1000 executions), which
cover all of NavierStokes' calls and all but 353 of Crypto's. The before
run did not print GC stats.

| | NavierStokes before | after | Crypto before | after |
|---|---|---|---|---|
| PutByVal helper calls | 63703218 | 38134941 | 96180700 | 1875827 |
| JSArray-tier inline hits | 3802990 | 52693843 | 11116836 | 182964842 |
| helper share of PutByVal | 94.4% | 42.0% | 89.6% | 1.0% |
| ValueBox (value needs a box) | 60773441 | 34177031 | 95338211 | 413176 |
| ... then BarrierMarking | 21071729 | 30579841 | 0 | 0 |
| ... then BarrierCompacting | 2569328 | 3597000 | 0 | 0 |
| ... then a failed guard | 0 | 0 | 237036 | 412538 |
| ... then Unexplained (every check passes) | 37132384 | 190 | 95101122 | 638 |
| not ValueBox: BarrierMarking / BarrierCompacting | 2232079 / 697698 | 2989716 / 968194 | 0 / 0 | 0 / 0 |
| not ValueBox: failed guard | 0 | 0 | 842489 | 1462651 |
| NumStoreBoxFallbacks | n/a | 190 | n/a | 638 |
| young collections | n/a | 323 | n/a | 667 |

After the change, "ValueBox then Unexplained" equals
`NumStoreBoxFallbacks` on both benchmarks, and both are below the number
of young collections. So the only helper calls declined because the
value needs a box are young-generation-full fallbacks, which is
criterion 1. The other helper calls that carry such a value (34.2M on
NavierStokes, 0.4M on Crypto) were declined before the box was needed:
by a failed guard, or by old-generation marking or a compaction being
active. Crypto's failed guards are `BelowBegin`,
`AtOrBeyondEnd` and `Hole`. Head does more work in the same time, so the
counts grow: NavierStokes ran 90.8M PutByVal stores, against 67.5M
before. Marking and compaction now account for every NavierStokes helper
call except the 190 fallbacks: 42.0% of its PutByVal stores, of which
boxing stores are 37.6%. Before, the boxing stores that also met an
active barrier were 35.0% of 67.5M. Those declines are the separate
change named in Non-goals, dz 01a18b30-a55b.

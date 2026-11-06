# JIT inline property writes (x86-64) — design spec

Date: 2026-08-27. Branch: `x86-jit`. Status: experiment; revertable.

## Motivation

On the React `pixel-grid` benchmark (precompiled bytecode, Release,
HV64, `-Xjit`), after the GC dead-zone fix, the write path is the
largest mutator-side cost cluster (~17%): `_jit_put_by_id` 7.6%
inclusive / 7.5% self, the `putByVal` cluster 9.4%, env stores ~1%.
Scratch counters show `_jit_put_by_id` was called 810M times on that
run, 99.5% of them write-cache HITS — i.e. the hit path (a hidden-class
compare and a slot store) runs in C++ behind an 8-argument call.

Root constraint: the JIT emits **no heap pointer store inline
anywhere** (`JitEmitter-object.cpp`: "TODO: Fix this once we can
inline write barriers"). `PutById`, `PutByVal`, `FastArrayStore` and
env stores are all helper calls, on both backends.

## Goal

Emit inline fast paths for named property writes (first), indexed
array element writes (second) and environment slot stores (third) on
x86-64, each guarded by an inline "safe store" predicate that performs
the store only when the write barrier is provably either a no-op or a
single card-dirty; everything else falls back to the existing helper,
which already implements the full barrier (snapshot + relocation).
No new runtime barrier code; concurrent-marking correctness stays in
C++.

Scorecard: `pixel-grid` (0.81x of V8 Sparkplug today), plus
`pixel-grid-small`, `relay`, and Octane `deltablue`/`richards` as
regression canaries. Decision after each stage whether to continue.

## Non-goals (this experiment)

- arm64 (helper-only there stays; a follow-up if worthwhile).
- HV32 / BOXED: the inline paths are compiled out under
  `HERMESVM_COMPRESSED_POINTERS` or `HERMESVM_BOXED_DOUBLES`; those
  builds keep the helper calls and must still pass all tests.
- Add-property transitions, dictionary-mode objects, accessor
  properties, proxies/host objects: all stay in the helper via the
  fallback.
- Inline snapshot (SATB) barrier: never inlined; marking → helper.

## The "safe store" predicate (barrier fast path)

From `HadesGC::writeBarrier(const GCSmallHermesValue *loc, SmallHermesValue v)`:

    if (inYoungGen(loc)) return;                    // (1)
    if (ogMarkingBarriers_) snapshotWriteBarrierInternal(*loc);   // (2)
    if (v.isPointer()) relocationWriteBarrier(loc, v.getPointer());  // (3)

and `relocationWriteBarrier`: dirty the card for `loc` iff
`inYoungGen(value) || (compactee_.contains(value) && !compactee_.contains(loc))`.

Inline decision, given `loc` (address of the slot) and `value` (64-bit
HermesValue) in registers, with `SEG = 1 << HERMESVM_LOG_HEAP_SEGMENT_SIZE`:

    segLoc = loc & ~(SEG-1)
    if (segLoc == [runtime.heap_.youngGen_.lowLim_])   → STORE, done   (young target: no barrier)
    if ([runtime.heap_.ogMarkingBarriers_] != 0)       → HELPER          (snapshot barrier needed)
    if (compactee active)                              → HELPER          (relocation barrier's compactee branch)
    if ([segLoc + 4] != 1)                             → HELPER          (jumbo segment: cards are out of line)
    STORE
    if (value is a pointer) {
      segVal = value.pointer & ~(SEG-1)
      if (segVal == [youngGen_.lowLim_])
        byte [segLoc + ((loc - segLoc) >> 9)] = 1     (dirty card; cards array is at segment offset 0)
    }

Facts this relies on (all verified in source):
- The YG is a single `FixedSizeHeapSegment`; its `lowLim_` can change
  (`HadesGC::setYoungGen` swaps segments), so it is LOADED at runtime
  via a new `RuntimeOffsets::runtimeHadesYGStart =
  offsetof(Runtime, heap_.youngGen_.lowLim_)` (siblings
  `runtimeHadesYGLevel`/`runtimeHadesYGEnd` already exist).
- `FixedSizeHeapSegment::Contents` places `inlineCardsArray_` in a
  union at offset 0 of the segment; `addressToCardIndex` is
  `(addr - segStart) >> kLogCardSize` with `kLogCardSize` = 9;
  `CardStatus::Dirty` = 1, one byte per card, relaxed atomic store
  (a plain byte store is the same instruction).
- `loc` must be in a fixed-size segment for the card math to be valid,
  and this needs BOTH an inline test and a caller obligation.
  (CORRECTED after implementation; the original claim that PutById's
  slots "always are" was wrong.) The write cache can hold a *cacheable
  dictionary* class: `Interpreter::putByIdSlowPath_RJS` declines to
  cache only when `clazz->isDictionaryNoCache()`. A dictionary object
  has no bound on its property count
  (`HiddenClass::maxNumProperties()` is `2^24-3`), so its `PropStorage`
  can be a jumbo cell — `PropStorage` is `CanBeLarge::Yes`, and
  `setNamedSlotValueIndirectUnsafe` → `ArrayStorage::set` →
  `writeBarrierForLargeObj` reaches the OUT-OF-LINE `cards_` array, not
  the inline one. `HiddenClass::createForTypedObject` is a second route
  to a non-dictionary class with an unbounded property count. So:
    * The predicate itself tests the segment size — the 16-bit
      `SHSegmentInfo::shiftedSegmentSize` at segment offset 4 must be 1
      — and sends anything else to the helper. Only a one-unit segment
      keeps its card array inline at offset 0.
    * That test is only meaningful if `loc` lies within the FIRST
      `kSegmentUnitSize` bytes of its segment. A `JumboHeapSegment` is
      aligned to `kSegmentUnitSize` but is N units long, so for a `loc`
      further in, `loc & ~(SEG-1)` names a later unit and the word at
      +4 is object payload; if it happens to be 1 the guard passes and
      the card byte lands in the cell's own data while the real card
      stays clean. Note the asymmetry that makes this bite: the runtime
      derives the segment start from the OWNING CELL
      (`dirtyCardForAddressInLargeObj(owningObj, loc)`), this code
      derives it from `loc`, and the two agree only under that bound.
    * Stage 1 satisfies the bound through
      `WritePropertyCacheEntry::kMaxSlot` (0xff), which places `loc` at
      most ~2KB past a cell head, and every cell head lives in the
      first unit of its segment. Stage 2 must satisfy it through the
      `kMaxInlineStorage` gate described below — that gate is REQUIRED,
      not an optimization, and the predicate's segment-size test does
      not replace it.
- Compactee detection: `HadesGC::CompacteeState` has a `start` pointer
  equal to a documented non-null `invalid` sentinel when no compactee
  exists (`empty()`), so the emitted check is `[runtime + offsetof(heap_.compactee_.start)] == invalid`
  (expose the sentinel via `RuntimeOffsets` or a constexpr; the
  implementer confirms the exact field/sentinel from `HadesGC.h:1270-1304`).
- `ogMarkingBarriers_` is already read inline by the emitter
  (`JitEmitter-object.cpp:266`, `runtimeHadesOGMarkingBarriers`).
- "value is a pointer": HermesValue tag >= HVTag_FirstPointer; the
  tag helpers in `JitEmitter-internal.h` cover this.

The predicate is a shared emitter helper:

    /// Emit: store `value` (64-bit HV in a GP reg) to `[loc]` if the write
    /// barrier is a no-op or a card-dirty; otherwise jump to `slowLab`
    /// WITHOUT storing. Clobbers the given temps and EFLAGS.
    void emitSafeStoreOrSlow(x86::Gp loc, x86::Gp value, x86::Gp t1, x86::Gp t2, asmjit::Label slowLab);

`loc` must lie within the first `kSegmentUnitSize` bytes of its
segment; see the facts above.

Under `HERMESVM_COMPRESSED_POINTERS` or `HERMESVM_BOXED_DOUBLES` the
consumers do not call it (they emit the helper call unconditionally,
exactly as today).

## Stage 1 — PutById inline tier (object specialization)

Mirror `GetByIdImpl::emitObjectSpecialization`: at compile time read the
write cache entry (`CodeBlock::getWriteCacheEntry(cacheIdx)`,
`WritePropertyCacheEntry{ WeakRoot<HiddenClass> clazz; slot in low 8
bits }`); if `clazz` is non-null and `initHCLazyIDMayAlloc(clazz)`
yields an id, emit:

    base is object?                                   else → helper
    hc = base->clazz (decode cp); hc->lazyJITId == id? else → helper
    loc = slot < HERMESVM_DIRECT_PROPERTY_SLOTS
          ? base + offsetof(SHJSObjectAndDirectProps, directProps) + slot*8
          : decode(base->propStorage) + offsetof(SHArrayStorageSmall, storage) + (slot-DIRECT)*8
    emitSafeStoreOrSlow(loc, value, ..., helperLab)
    (fallthrough = done)
  helperLab: existing 8-argument `_jit_put_by_id` call, unchanged.

If the cache is cold at compile time (e.g. `-Xjit=force` first call),
emit the helper call only — as today. Both `PutById` and `TryPutById`
variants and strict/loose flags are handled identically (the helper
receives them; the inline path is only taken for an existing own
data property of the cached class, where they are irrelevant).
The `usedHCs` rooting of the pinned class follows the read side.

Note: the write cache `clazz` is the class of the object for an
existing-property write (no transition), so equality proves the slot.

## Stage 2 — PutByVal inline fast array store

Mirror `JSObject::putComputedWithReceiver_RJS`'s first branch: if
`flags_.fastIndexProperties` and the key is an array index, the write is
`setOwnIndexed` → `JSArray::_setOwnIndexedImpl` in-range branch. Emit:

    base is object AND cellkind == JSArray                else → helper
    base->flags.fastIndexProperties set                   else → helper
    key is a number whose value is an exact uint32 idx    else → helper
    idx - beginIndex_ < elemCount_ (unsigned compare)     else → helper
    storage = decode(indexedStorage_); size(storage) <= kMaxInlineStorage  else → helper
    loc = storage + offsetof(SHArrayStorageSmall, storage) + (idx-begin)*8
    emitSafeStoreOrSlow(loc, value, ..., helperLab)

`kMaxInlineStorage` guarantees the storage cell lives in a fixed-size
segment (choose it so the cell's byte size is below the large-object
threshold; implementer derives the constant from
`FixedSizeHeapSegment::maxSize()` / the large-allocation threshold and
documents it). This gate is REQUIRED: `emitSafeStoreOrSlow`'s own
segment-size test assumes `loc` is in the first unit of its segment
(see the predicate's facts above), which a large array's indexed
storage does not satisfy on its own. The implementer must read `_setOwnIndexedImpl` and
confirm the in-range branch is a plain `set` with no other checks
(e.g. frozen/sealed are already reflected in `fastIndexProperties`);
if anything else is checked there, the inline path replicates it or
declines. Holes: storing over an `empty` slot in range is what
`_setOwnIndexedImpl` does too — confirm.

## Stage 3 — environment stores

`_sh_ljs_store_to_env(env, val, index)`: `loc = env + offsetof(SHEnvironment, slots) + index*8`,
then `emitSafeStoreOrSlow`. The `np` variant (`store_np_to_env`) never
needs a barrier and is a plain inline store.

## Testing requirements (each stage)

1. lit test under `test/jit/x86-64/` exercising the inline path for:
   young target; old target with young pointer value across forced
   young-gen collections (old object promoted first; then many stores of
   fresh objects + allocation pressure; then read back and verify) —
   the card-dirty path; old target with non-pointer value; the fallback
   when the class does not match (polymorphic site). Values verified
   against the interpreter (RUN with and without `-Xjit=force`, diff).
2. Prove-can-fail: with the card-dirty store temporarily disabled, the
   old→young test must fail (garbage / ASan report). Recorded in the
   task report, then reverted.
3. `LIT_FILTER="jit/"` green on cmake-build-x86jit (ASan HV64), -hv32,
   -boxed (helper path), -rel; `aarch64/jit-stress.js` differential
   byte-identical (G4); full `check-hermes` on the ASan tree at the end
   of each stage.
4. `-Xjit-emit-asserts` and `-Xjit-emit-type-asserts` runs of the new
   tests must pass.
5. Measurement: precompiled `pixel-grid`, `pixel-grid-small`, `relay`,
   interleaved before/after (binary A/B) — the stage is kept only if it
   is a win on pixel-grid with no regression elsewhere.

## Risks

- Baking GC geometry (segment size, card size, YG field offsets,
  compactee sentinel, marking flag) into emitted code: a maintenance
  contract, expressed through `RuntimeOffsets` like the existing YG
  bump-allocation fields. A concurrent GC branch exists
  (`gc-improvement`); any change there to these fields must update
  `RuntimeOffsets` or the static_asserts guarding them.
- Ordering: the marking-flag test must precede the store (the snapshot
  barrier reads the OLD value); the inline path never stores when the
  flag is set, so this holds by construction.
- Large objects: covered by the fixed-size-segment gates above.

## Stage 4 — HV32 / BOXED support (added 2026-08-27)

Stages 1-2 are compiled out under `HERMESVM_COMPRESSED_POINTERS` /
`HERMESVM_BOXED_DOUBLES`. Measurements showed nothing in this branch
improved HV32 (the GC dead-zone fix is a structural no-op there:
`minAllocationSize()` == the 8-byte bucket step), so the inline write
paths are the one lever available to the Android configuration.

What changes in those modes (verified in `SmallHermesValue.h`/
`-inline.h`): `SmallHermesValue` is `HermesValue32` whenever
`HERMESVM_BOXED_DOUBLES` is defined (both the hv32 and boxed trees);
its `RawType` is `CompressedPointer::RawType` (32-bit with compressed
pointers, pointer-width without), with `kNumTagBits = SH_SHV_TAG_BITS`
in the low bits and `kNumValueBits = kNumRawTypeBits - kNumTagBits`.
Encoding a 64-bit HermesValue (`HermesValue32::encodeHermesValue`):

- number-or-compressible (undefined/null/bool/empty and doubles): if
  `isShiftedUInt<kNumValueBits, 64 - kNumValueBits>(raw)` (the low
  `64 - kNumValueBits` bits are zero) then the SHV is
  `raw >> (64 - kNumRawTypeBits)` with tag `CompressedHV64` (== 0,
  static_asserted); otherwise a `BoxedDouble` must be ALLOCATED — the
  inline path DECLINES to the helper in that case.
- pointers (Object/Str/BigInt): tag remap (`HV32 Tag = HV64 Tag -
  (Tag::Str - Tag::String)`, static_asserted in the runtime) and
  `CompressedPointer::encodeNonNull(ptr) | tag` — the emitter already has
  `emit_sh_cp_encode_non_null`.
- symbols: `fromTagAndValue(Tag::Symbol, id)`.

Design: the encode lives INSIDE the predicate so consumers stay
mode-agnostic. `emitSafeStoreOrSlow(loc, value64, t1, t2, slowLab)`
under these modes: (1) young-target exit exactly as before (loc-based);
(2) marking / compactee / segment-size tests as before; (3) encode
`value64` into `t2` per the rules above, jumping to `slowLab` for the
BoxedDouble case (nothing stored yet, so declining is safe);
(4) store the SHV (32-bit under compressed pointers, otherwise raw
width); (5) the card-dirty decision uses the ORIGINAL 64-bit value:
pointer tag test on `value64` and young-segment compare on its raw
pointer (mask with `~(SEG-1)` against `youngGen_.lowLim_`) — equivalent
to the runtime's `inYoungGen(CompressedPointer)` (segment-start compare
against `youngGenCP_`). BoxedDouble never reaches the store, so
`value.isPointer()` on the SHV side never has to consider it.

Consumers: `emitPutByIdInlineTier` and the PutByVal tier already compute
`loc` via the mode-aware `sh_mirror.h` structs; their slot arithmetic
uses `sizeof(SHGCSmallHermesValue)` (check every hardcoded `*8`). The
`HERMES_JIT_INLINE_SAFE_STORE` gate drops the two mode exclusions.
`RuntimeOffsets` pins re-verified under both modes; add pins for
`SH_SHV_TAG_BITS`, `SH_SHV_RAW_TYPE_BITS`, the `CompressedHV64 == 0`
tag, and `sizeof(SHGCSmallHermesValue)`.

Testing: the existing `putbyid-inline*.js` / `putbyval-inline*.js` lose
their `REQUIRES: heap_hv_64` (the `-emitted` pins gain per-mode CHECK
prefixes like `test/jit/x86-64/hvmodes.js`), plus a new test that stores
every SHV shape through the inline path on an old object across
young-gen collections: undefined/null/true/false, small ints, a double
that is compressible, a double that is NOT (must take the helper and
box), strings, objects, a symbol, a bigint — verified against the
interpreter. Gates: jit suite on all four trees, G4 on hv32 and boxed,
full ASan `check-hermes` on the hv32 tree (it exists) and on the HV64
tree; prove-can-fail on the hv32 tree (card store removed → old→young
test fails). Measurement: HV32 Release trees (`cmake-build-x86jit-rel-hv32`
is HEAD; `.../scratchpad/hermes-hv32-head` is the pre-Stage-4 binary),
interleaved A/B on pixel-grid, pixel-grid-small, relay, Richards,
DeltaBlue, Box2D (precompiled .hbc in the scratchpad).

### Stage 4, corrections found during implementation

Three facts the design above did not anticipate, all of them in the
PutByVal tier rather than the predicate, and all of them consequences
of the slot width rather than of the encoding:

- The element address is a scaled index, not a byte offset. The `lea`
  was hardcoded to scale 3; under compressed pointers it must be 2.
  Expressed as `RuntimeOffsets::kLogSmallHermesValueSize` (log2 of
  `sizeof(SHGCSmallHermesValue)`), pinned by a `static_assert` that the
  slot is 4 or 8 bytes.
- The hole test read the slot as a HermesValue and compared its ETag
  against `HVETag_Empty`. That works wherever a slot is 8 bytes -- an
  inline value holds the HermesValue's bits unshifted, so BOXED without
  compressed pointers is fine too -- but not where it is 4, since the
  bits have been shifted down out of ETag position. Under compressed
  pointers the whole encoded empty value (`0xFFF90000`) is compared
  instead. `emit_shv_load_is_empty` in `JitEmitter-internal.h`.
- `KindAndSize` is as wide as a compressed pointer, and it packs an
  8-bit kind above the size. So the size field is 32 bits in an 8-byte
  header but only 24 in a 4-byte one, and the jumbo-cell gate's plain
  32-bit load at cell offset 0 has to mask the kind off before the
  `size - 1` compare. The `static_assert` that pinned the width at 32
  becomes `<= 32` plus that mask.

One design point the spec left open is settled the strict way: the
encode's dispatch ends with an equality test for `HVETag_Symbol` rather
than an unconditional "everything left is a symbol". `HVTag_RawHV32`
also reaches that arm and has no SmallHermesValue encoding at all
(`encodeHermesValue` asserts instead), so it is declined to the helper.
A JIT frame register should never hold one; declining costs an already
non-taken compare on the symbol path and nothing anywhere else.

Under `HERMESVM_SANITIZE_HANDLES` the encode declines every
number-or-compressible value rather than storing one inline. Handle-San
makes the runtime box even representable doubles, so that a
SmallHermesValue holding a number is always a pointer; emitted code
cannot allocate, so it stays out of the way rather than storing an
encoding the runtime would not have produced.

Per-mode emitted-code tests needed a mechanism that did not exist:
`REQUIRES: heap_hv_64` skips a whole file, which is the opposite of
what is wanted once the tier exists in all three modes. `test/lit.cfg`
gained a `%hv-mode` substitution naming the current mode, so a RUN line
reads `--check-prefixes=SPEC,SPEC-%hv-mode` and a file pins what is
common under `SPEC` and what differs under `SPEC-HV64` / `SPEC-HV32` /
`SPEC-BOXED`. FileCheck only errors when none of its prefixes has any
check, so the two inactive prefixes cost nothing.

### Stage 4b — encode before the guards (measured)

The first Stage 4 landing put the encode inside `emitSafeStoreOrSlow`,
which is the last thing a tier does. That made a store whose value has
no inline encoding pay for the entire guard chain before being declined.
A scratch counter over the HV32 Release build, incremented at each tier
entry and at the encoder's decline edge, showed how much that costs:

    benchmark    tier entries   declines      rate
    Box2D          17,380,706   11,061,828   63.64%
    relay           4,771,261      120,991    2.54%
    pixel-grid    981,338,804       61,674    0.01%
    pixel-grid-sm  66,729,631        6,378    0.01%
    Richards       85,299,788            0    0.00%
    DeltaBlue      15,365,166            0    0.00%

So the Box2D regression was exactly what it looked like: two thirds of
its stores are doubles with no inline SmallHermesValue encoding.

The encode therefore moved to the top of each tier, through a
mode-selecting wrapper (`emit_shv_encode_for_slot_or_slow`) that emits
nothing in `HEAP_HV_64` and returns the HermesValue itself, so the tiers
still carry no `#ifdef`. `emitSafeStoreOrSlow` now takes the encoded
value alongside the original; the original is what the card decision
still reads. The encoded value lives in `temp2`, which both tiers
already allocate and no guard touches, so holding it across the guards
is free; inside the predicate `temp2` is reused only after the store,
where the encoded value is dead.

Measured (HV32 Release, `-Xjit`, interleaved, encode-in-predicate vs
encode-first): Box2D +1.4% (n=12), pixel-grid +2.4% (n=5), Richards
+0.6%, pixel-grid-small +0.6%, DeltaBlue -0.3%, relay -1.0% (the last
two inside their run-to-run spread). Against the pre-Stage-4 build the
net is pixel-grid +6.5%, Richards +9.6%, pixel-grid-small +1.0%,
DeltaBlue +1.5%, relay -0.8%, Box2D -1.1%.

Box2D still loses. What is left is the encode itself on every store plus
the tier prologue on stores that fail a guard rather than the encode.
Removing that needs a per-site decline record, so a site that always
declines can stop running the tier; that is a follow-up.

## Stage 5 — arm64 port (added 2026-09-02)

Port the final x86-64 state to arm64: `emitSafeStoreOrSlow`, the PutById
and PutByVal tiers, and the SmallHermesValue encoder, in three commits
mirroring the x86-64 series (reference the x86-64 commits by subject and
short hash in each message; include an arm64 assembly example dumped via
`-Xdump-jitcode=3` under qemu; keep the rest of the message brief).
Performance cannot be measured (qemu-user only); correctness can and
must be.

Design carries over unchanged — the predicate's decision sequence, the
first-unit precondition, kMaxSlot / kMaxInlineStorage, the encode's
per-tag rules and its decline case are all architecture-independent, and
`RuntimeOffsets` already pins the geometry for both backends. What is
arch-specific is instruction selection only:

- Use the arm64 emitter's existing idioms and helpers (the x86-64 tiers
  were ported FROM arm64 originally, so most primitives exist there
  under the same names: `emit_sh_ljs_is_object`, tag helpers,
  `emit_sh_cp_encode/decode`, `emit_load_shv`/`emit_store_shv`, and the
  register-file API). Tag tests are `asr #47/#48` + `cmn`/`cmp`;
  wide masks come from `movk` chains or `loadBits64InGp`; the card
  dirty is a `strb` of wzr+1-style immediate via a temp; the young-gen
  and flag loads use the same RuntimeOffsets entries the arm64
  young-gen bump allocator already reads. x16/x17 remain non-allocated
  scratch, as elsewhere in the backend.
- The double→uint32 index test mirrors the runtime the same way x86
  does: `fcvtzu` / `ucvtf` / `fcmp`, rejecting NaN via the unordered
  result, then the `0xFFFFFFFF` sentinel check.
- Comment-for-comment parallels with the x86-64 files, matching the
  backends' established mirroring, with arm64-specific sentences where
  codegen genuinely differs (as the x86-64 files do in reverse).

Tests: the three behavioral tests (`putbyid-inline.js`,
`putbyval-inline.js`, `inline-store-shv-shapes.js`) are architecture-
independent — MOVE them from `test/jit/x86-64/` to `test/jit/` so both
backends run them (adjust doc references). The `-emitted` pin tests stay
x86-64; each arm64 stage adds its own pin test under `test/jit/` with
`REQUIRES: jit-arch-arm64`, following the same SPEC / SPEC-%hv-mode
prefix scheme.

Gates per stage: the arm64 jit suite under qemu; the
`aarch64/jit-stress.js` differential on the arm64 tree (threshold-mode
variants included where the tier needs a warm cache); prove-can-fail on
arm64 (delete the card-dirty store, the behavioral tests must fail on
verifyCardTable under the Debug tree); AND the x86-64 side must be
untouched — jit suite green on the four x86 trees and `jit-diff.sh`
byte-identical on the x86-64 HV64 dump corpus after every stage (shared
headers change; x86 emission must not).

Stage 5c (the encoder) additionally requires new arm64 cross trees for
the other heap modes — `cmake-build-arm64-hv32` and
`cmake-build-arm64-boxed`, configured like `cmake-build-arm64` (same
toolchain file, `HERMES_UNICODE_LITE`, `IMPORT_HOST_COMPILERS`,
`QEMU_RUN_PREFIX`) plus the `HERMESVM_HEAP_HV_MODE` setting — and green
jit suites there. It closes dz 01a04e00-07fc in the same commit, updates
doc/JIT.md where it says the inline write path is x86-64-only (including
the arm64 "TODO: Fix this once we can inline write barriers" comment),
and drops the gate's arch restriction (`HERMES_JIT_INLINE_SAFE_STORE`
becomes arch-independent under Hades).

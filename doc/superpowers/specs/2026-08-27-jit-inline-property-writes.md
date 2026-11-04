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

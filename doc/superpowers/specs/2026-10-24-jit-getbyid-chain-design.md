# JIT: GetById chain entries for properties two or more prototypes up — design

Date: 2026-10-24. Status: reviewed (Codex, 4 rounds: 3 blockers, 5 major
and 4 minor findings, all closed). Amended after the rebase onto
adee03e65, which fixed `findPropertyNoAlloc`: the walk now falls back to
it, and its "Unknown" result is gone.
Scope: x86-64 and arm64. Issue: dz `01a13df8-970b`.
Extends doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md
(the "poly spec"); everything not restated here is unchanged from it.

## Motivation

Measured at e5c158faa (dz 970b log): DeltaBlue makes 31.8M slow GetById
calls from 55 sites; 95.7% find the property two prototypes up, 3.0%
three up. The depth comes from `inheritsFrom`
(benchmarks/octane/deltablue.js:439-447), which inserts an `Inheriter`
instance between every subclass prototype and its superclass prototype.
The poly tier harvests the interpreter read cache, which caches nothing
deeper than the immediate prototype, so these sites record nothing,
demote, and walk the chain in C++ on every call (~30% of DeltaBlue's
cycles, dz 01a13c62). Since 86cdffc84 ("Copy parent to HiddenClass",
PR #1939) that walk also pays an extra indirection per level: DeltaBlue
lost 5–6% in both the interpreter and the JIT.

Goal: inline hits for data properties at depth 2 to `kMaxChainDepth`
(8). The guard costs the same at any depth; reaching the holder costs
two dependent loads per level beyond the first. Depth 0 and 1 keep the
poly spec's own and proto entries.

## The validity argument

A **chain entry** is (receiver class, depth, slot, epoch). A hit requires
the receiver's class to match and the runtime's parent cache epoch to
equal the entry's epoch. The emitted code then follows the live chain
`depth` levels from the receiver and loads `slot` from the object it
reaches. This is sound because:

1. The receiver's class fixes the receiver's own properties and, since
   #1939, its prototype: `HiddenClass::objectParent_` is a strong field
   (lib/VM/HiddenClass.cpp:129).
2. Every object from the receiver's prototype up to and including the
   holder carries `isCachedUsingEpoch` (a class flag, HiddenClass.h:118,
   :551). On a flagged object, every class change bumps the epoch
   (`JSObject::updateClass`, include/hermes/VM/JSObject.h:1447-1459), and
   so does every prototype change (`JSObject::setParent`,
   lib/VM/JSObject.cpp:313-315).
3. Hence any change that could alter the lookup's answer bumps the
   epoch: a shadowing property added to an intermediate object, deletion
   or reconfiguration of the holder's property (a flags transition,
   HiddenClass.cpp:624-681, or conversion to a dictionary), or a
   prototype swap anywhere on the path. Writing a new value to the
   holder's property does not change its class; the emitted code reads
   the slot at run time, so it sees the write.
4. The flag survives later transitions: child classes copy `flags_`
   (HiddenClass.cpp:527 for additions, :672 for flags transitions, :869
   for parent changes).
5. The only way to change an object's class without `updateClass` is
   `updateClassIgnoringEpochUnsafe`, called only by flagging
   (JSObject.cpp:2971). `setEmptyClassUnsafe` goes through `updateClass`
   (JSObject.cpp:122) and is used only during runtime bootstrap
   (Runtime.cpp:449). No path makes a
   non-lazy object lazy; materialization replaces a lazy class with a
   non-lazy one (Callable.cpp:134).
6. Objects whose lookup is not described by a class that only
   `updateClass` changes are excluded from the path: dictionaries
   (in-place mutation), proxies and host objects, lazy objects. These
   are `tryCacheAddProperty`'s exclusions (JSObject.cpp:3060-3075), plus
   lazy objects, which it materializes instead. Typed objects never
   change class or parent and are never flagged (`setCachedUsingEpoch`,
   JSObject.cpp:2955-2960); the walk treats them as flagged.
7. The receiver must be a plain object too: not a dictionary (it could
   gain the property without a class change), not lazy, host or proxy.
8. For the objects it admits, the validating walk performs exactly the
   lookup `getNamedDescriptorUnsafe` performs: a class lookup per level
   (JSObject.cpp:1028-1098).

The emitted traversal dereferences only objects reachable from the
receiver it holds, through strong fields, inside a sequence with no
safepoint, so no weak reference, barrier or GC hook is involved.

## The epoch becomes 64 bits

Today `parentCacheEpoch_` is `uint32_t` (include/hermes/VM/Runtime.h:1464)
and wraps at 2^24−1 to 1 (`incParentCacheEpoch`, Runtime.h:717-732),
because `AddPropertyCacheEntry` packs it into 24 bits next to an 8-bit
slot (include/hermes/VM/PropertyCache.h:146-177). A wrapping epoch would
let a stale chain entry match again. Chain entries have no size
pressure, so the counter is widened instead of adding a second one:

```cpp
/// Monotonic; never wraps in practice. The add cache uses its low 24 bits.
uint64_t parentCacheEpoch_ = 1;
/// Compared against the full counter.
uint64_t arrayFastPathParentCacheEpoch_ = 0;

/// \return the low 24 bits, as stored in AddPropertyCacheEntry.
uint32_t getParentCacheEpoch() const {
  return parentCacheEpoch_ & AddPropertyCacheEntry::kMaxParentEpoch;
}
/// \return the full epoch.
uint64_t getFullParentCacheEpoch() const { return parentCacheEpoch_; }

uint32_t incParentCacheEpoch() {
  ++parentCacheEpoch_;
  if (LLVM_UNLIKELY(
          (parentCacheEpoch_ & AddPropertyCacheEntry::kMaxParentEpoch) ==
          AddPropertyCacheEntry::kMaxParentEpoch)) {
    // The low 24 bits wrapped: invalidate the add cache as before and
    // skip ahead so they restart at 1 (0 is never a valid add epoch).
    invalidateAllAddCacheEntries();
    parentCacheEpoch_ += 2;
  }
  return getParentCacheEpoch();
}
```

- The add cache's readers (JSObject.cpp:3073, Interpreter.cpp:2079,
  StaticH.cpp:812, JitHandlers.cpp:391) keep calling
  `getParentCacheEpoch()` and see exactly today's sequence of 24-bit
  values.
- The array fast path (`checkArrayFastPathParentEpoch`,
  `setArrayFastPathParentEpoch`, Runtime.h:696-708; used at
  lib/VM/JSLib/Array.cpp:454, :540) compares the full counters; the
  zeroing on wrap goes away and its assert (`<=`) holds by monotonicity.
- `testSetParentCacheEpoch` (Runtime.h:710-715) sets the full counter.
  `ParentCacheEpochTest` (unittests/VMRuntime/ObjectModelTest.cpp:888-917)
  must pass unchanged, and gains a check that the full counter kept
  increasing across the wrap.
- `RuntimeOffsets` (lib/VM/JIT/RuntimeOffsets.h) gains
  `runtimeParentCacheEpoch = offsetof(Runtime, parentCacheEpoch_)` with
  `static_assert(sizeof(Runtime::parentCacheEpoch_) == 8)`; Runtime.h:1336
  already declares `friend struct RuntimeOffsets`. arm64 also
  `static_assert`s that it is below `kMaxInlineBaseOffset`, as
  JitEmitter-internal.cpp:514 does for the Hades fields.

## Records

`JitByIdShapeEntry` (include/hermes/VM/JIT/JitFunctionData.h:109-151):

```cpp
enum class JitByIdEntryKind : uint8_t { Own, Proto, Chain };

struct JitByIdShapeEntry {
  WeakRoot<HiddenClass> objClass{nullptr};
  /// Proto only.
  WeakRoot<HiddenClass> protoClass{nullptr};
  /// Chain only: the full parent cache epoch at which a walk last
  /// validated this entry. Read by emitted code; refreshed in place.
  uint64_t epoch = 0;
  /// Chain only: the epoch at which revalidating this entry last failed
  /// for a semantic reason; revalidation skips the entry until the
  /// epoch moves.
  uint64_t refreshFailedAt = 0;
  uint16_t slot = 0;
  /// Chain only: 2..kMaxChainDepth.
  uint8_t depth = 0;
  JitByIdEntryKind kind = JitByIdEntryKind::Own;   // replaces isProto
  uint8_t emitted = 0;
  uint8_t unemittable = 0;
  ...
};
```

- `isProto` becomes `kind`, set once at recording and never changed.
  The hand-written copy operations (JitFunctionData.h:133-145) copy the
  new fields.
- **Live:** own and chain — `objClass`; proto — `objClass && protoClass`.
- **Identity** (dedup in `addByIdShape`, JitHandlers.cpp:553-566):
  kind, `objClass`, `protoClass` for proto, `depth` for chain, `slot`.
  The epochs are not part of identity. Holder identity is not needed:
  receiver class, depth and slot, validated at an epoch, determine the
  answer.
- **No overwrite of emitted entries.** `addByIdShape` overwrites the
  first dead entry (JitHandlers.cpp:574-578); it now skips emitted
  entries. No behavior changes today (emitted classes are pinned, so
  emitted entries never die); it states the invariant the stable
  addresses below need.
- **Stable addresses.** Emitted code embeds the address of an entry's
  `epoch`. Records are heap-allocated through `std::unique_ptr`
  (JitFunctionData.h:195); entries live in the record's
  `SmallVector<…, kMaxEntries>` inline storage (:165), which never
  reallocates because `addByIdShape` never exceeds `kMaxEntries`
  (:579-582; an assert states it). Entries are never erased or
  reordered (see Retirement).

## The walk

New in lib/VM/JIT/JitHandlers.{h,cpp}, declared in the header so a unit
test can call it:

```cpp
struct JitChainWalk {
  enum class Status : uint8_t {
    Found,     ///< data property at depth >= 2, slot <= kMaxSlot
    Rejected,  ///< not cacheable as a chain entry
  };
  Status status;
  JSObject *holder;   ///< Found only
  SlotIndex slot;     ///< Found only
  uint8_t depth;      ///< Found only
  /// Every object the walk visited (receiver's prototype up to the
  /// holder, or up to where it stopped) is flagged or typed; false if it
  /// stopped at a dictionary, proxy, host or lazy object. For a
  /// Rejected or Found result this says whether the verdict is fixed
  /// until the epoch moves.
  bool allFlagged;
};
/// Never allocates.
JitChainWalk jitWalkChain(Runtime &, JSObject *obj, SymbolID name);
```

- Rejected if the receiver or any object on the path is a dictionary,
  lazy, host or proxy object; if the property is found on the receiver
  or at depth 1; if it is found as an accessor or with the
  `hostObject`/`proxyObject` descriptor flags; if `desc.slot >
  ReadPropertyCacheEntry::kMaxSlot` (0xffff, PropertyCache.h:97, the
  same limit as the read cache fill, JSObject.cpp:1207); if it is not
  found by depth `kMaxChainDepth`; if it is not found at all.
- Per level it uses `HiddenClass::tryFindPropertyFast`
  (HiddenClass.h:912-925) and, when that returns None (the class has no
  property map), falls back to `HiddenClass::findPropertyNoAlloc`
  (HiddenClass.cpp:365-406). Maps go missing when a transition steals
  them (HiddenClass.cpp:565-568) or a flag transition creates a mapless
  class (:790-796), so the fallback is needed for the walk to give the
  same answer as the full lookup without allocating. Since adee03e65 the
  fallback is correct after flags transitions: it takes the newest flags
  transition's flags and the slot from the class that added the
  property, or from a map (it used to report `numProperties_ - 1`, but a
  flags transition keeps the property's slot, :677; dz `01a170e9`). For
  the non-dictionary classes the walk admits, every lookup therefore has
  a definite answer.

## Recording and refresh

**Fast refresh** (in a `NoAllocScope`, before forwarding). Candidates:
live chain entries whose `objClass` equals the receiver's class and
whose `epoch` and `refreshFailedAt` both differ from
`getFullParentCacheEpoch()`. If there is a candidate, walk. If the walk
is Found with `allFlagged` and a candidate has the found (depth, slot),
set its epoch to the current epoch. If that entry is emitted, return
the holder's slot value (`getNamedSlotValueUnsafe(holder, runtime,
slot)`, unboxed) without forwarding and without counting a decline. If
it is not emitted yet, go on to forward and count as usual: completing
the call early would hide the miss from the recompile trigger, and in a
program that bumps the epoch between misses the entry would never be
emitted. Otherwise do nothing and go on. Counted as
`NumByIdChainRefreshes`. This is what keeps a program that bumps the
epoch constantly at about today's cost: one walk per miss.

**Revalidation** (after forwarding, with the result rooted). It walks
again. If Found and an entry with
identity (Chain, receiver class, depth, slot) exists whose epoch is
stale: read `E0`, flag the path, and if the epoch is still `E0` set the
entry's epoch to `E0`. This covers what the fast refresh cannot do
without allocating: a path that now contains unflagged objects. If the
walk is Found with a different (depth, slot), or Rejected, and
`allFlagged` holds, set `refreshFailedAt` to the current epoch on every
stale candidate: every object the verdict depends on is flagged, so it
cannot change without a bump. A verdict that depends on an unflagged
object (or on a dictionary, proxy, host or lazy object, which stops the
walk) can change without a bump, so it suppresses nothing and the next
miss retries; until the path settles, each miss for that receiver class
costs two extra walks (the fast refresh before forwarding and the
revalidation after it) on top of the forwarded lookup. Revalidation
never sets `changed`.

**Flagging the path** (shared by revalidation and recording): from the
rooted receiver, a rooted cursor follows `getParent` for `depth` levels
and calls `JSObject::setCachedUsingEpoch` on each unflagged, untyped
object, as `tryCacheAddProperty` does (JSObject.cpp:3104-3113). Flagging
changes only class flags (`markAsCachedUsingEpoch`, HiddenClass.h:558-
570; the new class keeps parent, property count and ancestry,
HiddenClass.cpp:780-796), cannot run JS and cannot throw, so the walk's
(depth, slot) stays valid and no `LocalsRAII` spans a throwing call.
It can allocate and collect: nothing raw crosses it except the scalars
depth, slot and `E0`; the receiver's class is read from the rooted
receiver afterwards. The epoch check catches anything unexpected.

**`_jit_get_by_id`** (JitHandlers.cpp:632-668) becomes:

0. If `vd` is not `current` or `site->helper != _jit_get_by_id`, skip
   steps 1, 3 and 4 (today's staleness rule, :660-662, now also applied
   before the fast refresh).
1. Fast refresh.
2. Forward and root the result (:649-656), as today.
3. Re-check staleness; harvest from the read cache
   (`recordByIdObservation`, :592-630), returning whether it harvested.
4. If nothing was harvested and the source is an object, walk:
   - Found, identity exists: revalidation, as above.
   - Found, new identity, room (fewer than `kMaxEntries` entries or a
     dead unemitted one): read `E0`, flag the path, and if the epoch is
     still `E0`, `addByIdShape` with kind Chain, `epoch = E0`. A new
     entry sets `changed`, as for the other kinds.
   - Found, new identity, no room: `addByIdShape` without flagging, so
     its overflow branch sets `megamorphic` and `changed` once
     (JitHandlers.cpp:583-586). Nothing is stored.
   - Rejected: the `refreshFailedAt` update of revalidation.
5. Count the decline (:664-666), as today.

Flagging changes each newly flagged prototype's class once, so read
cache entries and depth-1 entries keyed on those classes miss once and
re-record.

**`_jit_get_by_id_refresh`** (new, same signature as `_jit_get_by_id`):
if `vd` is `current`: fast refresh, forward, revalidation (only if a
candidate exists). Otherwise forward. No recording, no counting.

## Demotion, retirement, carry-forward

- **Demotion** (the ById pass in `considerRecompile`,
  JitCompiler.cpp:435-448): a site that meets the existing rule flips
  to `_jit_get_by_id_refresh` if it holds an emitted chain entry, else
  to `_sh_ljs_get_by_id_rjs` as today. Both count `NumByIdDemotions`
  and decrement `recordingSites`. `_jit_get_by_id_refresh` is not a
  recording helper (`isRecordingHelper`, :342-346, unchanged). Without
  this, one epoch bump after demotion would disable a site's chain
  entries for good.
- **Retirement** (`demoteAllSitesOnRetirement`, JitCompiler.cpp:375-391)
  stops clearing entries (:386-388): a retired body can still be
  running (test/jit/getbyid-poly-retire-reenter.js) and reads its chain
  entries' epochs. Instead it nulls every entry's weak class roots and
  keeps the scalars, and flips both `_jit_get_by_id` and
  `_jit_get_by_id_refresh` slots to the plain helper. A retired body's
  chain entries stay correct by the validity argument and stop hitting
  after the next bump, since nothing refreshes them. Retired records
  stay untraced; the debug check in `markWeakElementsInCaches`
  (CodeBlock.cpp:368-379) changes from "entries empty" to "every
  entry's weak roots are null". Emitted receiver IDs are immediates
  (x86 JitEmitter-property.cpp:1136), so nulling the roots does not
  affect running code.
- **Carry-forward** (Compiler constructor, JitCompiler.cpp:125-157):
  chain entries copy whole; `refreshFailedAt` resets. The new body
  embeds the addresses of its own copies.
- **Progress** (`byIdSiteCanProgress`, :314-319): unchanged in form; a
  live, unemitted, emittable chain entry is progress.

## Emission

`preparePolyChain` (x86 JitEmitter-property.cpp:1085-1115, arm64
:1178-1208): a chain entry needs a lazy ID for `objClass` only.
`PolyEntry` gains `kind`, `depth` and, for chain, the address of the
record entry's `epoch`. Order: own entries, chain entries, then the
proto block. Chain entries come before the proto block so they leave
its state untouched.

**x86-64.** Entry state as today: `temp1` object, `temp2` compressed
receiver class, `temp3` receiver lazy ID after the prologue
(:1125-1128). Per chain entry:

```
  cmp   temp3d, ID_recv
  jne   next
  mov   r11, imm64 &entry.epoch                   ; xScratch
  mov   temp4, [r11]
  cmp   temp4, [r15 + runtimeParentCacheEpoch]    ; xRuntime
  jne   next
  ; both guards passed; from here on the path ends at contLab
  emitIncrementCounter(NumByIdChainHits)          ; r11, EFLAGS; only
                                                  ; with -Xjit-emit-counters
  reg = emit_sh_cp_decode_non_null_preserve_input(temp4, temp2)
  emit_load_cp temp4, [reg + hiddenClassObjectParent]
  emit_sh_cp_decode_non_null temp4                ; temp4 = P1
  repeat depth-1 times:
    emit_load_cp temp4, [temp4 + clazz] ; decode
    emit_load_cp temp4, [temp4 + hiddenClassObjectParent] ; decode
  emitLoadFromSlot(hwRes, temp4, slot) ; emitFirstCase ; jmp contLab
next:
```

- On a miss (either `jne`), `temp1`, `temp2` and `temp3` are untouched,
  so the next entry, the proto block and the generic join see the poly
  spec's state.
- `emit_sh_cp_decode_non_null_preserve_input` returns `temp2` itself in
  uncompressed modes (x86 JitEmitter-internal.h:382-396); the load uses
  the returned register, as the proto block does (:1146-1150).
- r11 is live from its `mov` to the epoch load; the counter increment
  reuses it afterwards. No decode or load helper uses it in any heap
  mode (JitEmitter-internal.h:333-339, :426-436;
  JitEmitter-property.cpp:881-899); `emitRestCases` clobbers it in
  boxed-double builds (JitEmitter-internal.cpp:92-98), after it is
  dead. `inc` writes EFLAGS where none are live.
- `hwRes` may alias a temp (JitEmitter-property.cpp:940-942); it is
  written only after both guards, on the path to `contLab`, as for
  proto entries.

**arm64.** Same order, state and guards:

```
  emit_cmp_imm32(xTemp3.w(), ID_recv, w16) ; b.ne next
  loadBits64InGp(x17, &entry.epoch)           ; mov, or RO-data ldr
  ldr   xTemp4, [x17]
  ldr   x16, [x19, #runtimeParentCacheEpoch]
  cmp   xTemp4, x16 ; b.ne next
  emitIncrementCounter(NumByIdChainHits)      ; saves x0/x1 itself
  traversal as on x86 (decode helpers, arm64 JitEmitter-internal.h:498-540)
  emitLoadFromSlot(hwRes, xTemp4, xTemp2, slot) ; emitFirstCase ; b contLab
next:
```

x16 is free after the ID compare; x17 is used by nothing else in
`GetByIdImpl` (JitEmitter.h:205-209). `xTemp2` as the slot load's
temporary is clobbered only after both guards, as the proto block does
(arm64 JitEmitter-property.cpp:1261).

Size on x86-64: about 36 bytes per chain entry plus about 10 per level
beyond the first; at most `kMaxEntries` entries per site. Chain entries
sit inside the existing `// Get polymorphic chain (N entries)` block; N
counts them.

## Counters and dump

- `JIT_COUNTERS` (include/hermes/VM/JIT/JitCounters.h:18-33) gains
  `NumByIdChainRefreshes` (fast refreshes plus revalidations that set
  an epoch) and `NumByIdChainHits` (emitted on chain hit paths only
  with `-Xjit-emit-counters`, like `NumCall`, x86 JitEmitter.cpp:587-602,
  arm64 :656-670).
- The `-Xdump-jitcode` summary (JitCompiler.cpp:712-727) becomes
  `JIT ById sites: R recorded, P polymorphic, C chain, M megamorphic`,
  `C` = sites whose body emitted at least one chain entry. Existing
  tests' `DUMP` lines are updated.

## Non-goals

- Accessors (getters) at any depth; missing properties (negative
  caching through the chain); primitive receivers (dz 01a16b01).
- `GetByIdWithReceiver*` and `TryGetById*`.
- Chain entries for depth 1 (the proto entry covers it) or beyond
  `kMaxChainDepth`.

## Review focus

The inputs most likely to break this, each pinned by a test below:

1. A mutation on the path after the entry is emitted (shadowing add on
   an intermediate prototype, `setPrototypeOf`, holder property delete
   or redefinition as a getter or with new flags) — must read the new
   answer.
2. A path changed to new, unflagged objects with the property at the
   same depth and slot — must revalidate and hit again.
3. An unrelated epoch bump — must refresh once and hit again, also
   after demotion, also when a class on the path lost its map.
4. A retired body still on the stack — must hit while valid and fall
   through after a bump.
5. Dictionary and proxy objects on the path, dictionary receivers,
   slots above 0xffff, depth 9 — must not record.

## Testing

New lit tests in test/jit/, with the conventions of getbyid-poly-*.js
(`-fno-inline -Xjit=force -Xjit-crash-on-error
-Xjit-recompile-threshold=16`, `main()` wrapper, functions passed as
parameters, `REQUIRES: jit`), run on x86-64 and the three qemu arm64
trees. Execution evidence is `NumByIdChainHits` with an exact expected
count, derived in the test's comments; before the feature it is absent
(0). Each test also checks values.

- **getbyid-chain.js:** three receiver classes, methods two prototypes
  up (the DeltaBlue shape). DUMP `1 chain`; CNT exact
  `NumByIdChainHits`, `NumByIdDemotions: 0`.
- **getbyid-chain-depth.js:** holders at depth 2, 3, 8 (chain hits) and
  9 (no chain entry, values correct); depth 1 gets a proto entry.
- **getbyid-chain-invalidate.js:** after emission: shadowing add on the
  intermediate prototype; `setPrototypeOf` on it; delete the holder's
  property; redefine it as a getter; make it non-writable (a flags
  transition); assign a new value. Values correct after each; the
  value-write case keeps hitting (hit count) without a refresh.
  Removing the emitted epoch compare must make it fail.
- **getbyid-chain-refresh.js:** after emission, bump the epoch by adding
  a property to an unrelated flagged object: `NumByIdChainRefreshes: 1`
  and hits resume (count). Variants: (a) the site demotes first and the
  refresh goes through `_jit_get_by_id_refresh`; (b) replace the
  intermediate prototype's parent with a new unflagged object holding
  the property at the same depth and slot: revalidation, hits resume;
  (c) before recording, the holder's property `p` (not the last one
  added) goes through a flags transition (made non-enumerable) into
  class C; a sibling object is built the same way and stays on C.
  Recording moves the holder to a mapless flagged child of C
  (HiddenClass.cpp:790-796). After emission the sibling adds one more
  property, which moves C's map to its child (:565-568); then the
  epoch is bumped. The fast refresh walks the holder's two mapless
  classes, reaches C's flags transition in `findPropertyNoAlloc`, and
  must refresh with `p`'s real slot on the first miss: value correct,
  `NumByIdChainRefreshes: 1`. Reverting adee03e65 makes it fail;
  (d) after demotion, replace the intermediate's parent with an
  unflagged empty object (a miss, Rejected on an unflagged path), then
  define the property on it at the same slot without any further bump:
  hits resume (count).
- **getbyid-chain-mega.js:** five chain shapes at one site: DUMP
  `1 megamorphic`, four chain entries emitted.
- **getbyid-chain-exclude.js:** a dictionary prototype on the path, a
  proxy on the path, a dictionary receiver: DUMP `0 chain`. A function
  receiver (not materialized beforehand) whose own `name` shadows a
  `name` two prototypes up: the own value wins.
- **getbyid-chain-retire.js:** a recursion whose outer frames run a body
  with emitted chain entries (DUMP), recompiled underneath (as
  getbyid-poly-retire-reenter.js); outer frames read before and after
  an epoch bump plus a shadowing mutation. Values correct, and the hit
  count includes the outer frames' pre-bump reads, so forcing retired
  chain checks to miss fails the test.
- **Unit (unittests/VMRuntime):** `ParentCacheEpochTest` extension;
  `jitWalkChain` on a typed holder class with 65535 private slots
  reserved (`addNewTypedPrivateProperty`, HiddenClass.h:697) and two
  public properties at slots 0xffff and 0x10000, reached at depth 2:
  Found for the first, Rejected for the second.

Existing suites: check-hermes in cmake-build-x86jit (ASan, JIT=2) and
the hv32 and boxed x86 trees; the jit lit suite and qemu-sanity in the
three arm64 trees.

## Benchmarks

Octane from precompiled .hbc, `taskset -c 2`, interleaved reps, median
with min–max; JIT (cmake-build-jit2) and interpreter (cmake-build-release)
against the current tip. Expected: DeltaBlue's slow GetById calls drop
by more than 90% (re-measured with the dz 970b instrumentation) and its
JIT score rises well beyond noise; interpreter scores unchanged within
noise (only the epoch widening touches them); no JIT benchmark regresses
beyond noise. Record sizes of `JitByIdShapeEntry` and
`JitByIdSiteRecord` for HV64 and HV32 in the delivered section.

## Docs

- doc/JIT.md: the poly-chain text under "Inline caches" (:350-397)
  gains chain entries; "GetById shape records" (:1131-1228) gains the
  chain kind, the walk, refresh and revalidation, the refresh helper,
  retirement and the 64-bit epoch; "Debugging aids" (:1307) lists the
  new counters.

## Delivered (2026-10-25)

Tasks 1-9 of doc/superpowers/plans/2026-10-24-jit-getbyid-chain.md on
branch `x86-jit`, base `d2d2c226e`: the 64-bit epoch (Task 1), the entry
kinds (2), the walk (3), recording and refresh (4), x86-64 emission (5),
arm64 emission (6), the refresh, retirement and rooting tests (7),
doc/JIT.md (8) and these measurements (9).

### Differences from the text above

- **"Harvested"** (step 3 of `_jit_get_by_id`) means that the own or
  prototype branch of `recordByIdObservation` offered a shape to
  `addByIdShape`. The spec did not define it, and step 4 walks only
  when nothing was harvested.
- **The dump's chain count** comes from a new per-site byte,
  `JitByIdSiteRecord::chainEntryEmitted`, set when the body emits at
  least one chain entry at the site and reset on carry-forward next to
  `chainEmitted`. The existing `chainEmitted` says that a poly chain was
  emitted, which a site with only own and proto entries also sets, so it
  could not give `C`.
- **`NumByIdChainRefreshes`** is counted in C++ (the fast refresh and
  revalidation run in the helpers, not in emitted code), through a new
  `JITContext::incrementCounter`, a no-op when counters are off, on both
  arches.
- **Interim states.** Task 4 recorded and refreshed chain entries but
  emitted none (`preparePolyChain` skipped them and
  `byIdSiteCanProgress` did not count them as progress, so the new
  entries could not cause recompiles before they could be emitted).
  Task 5 emitted them on x86-64 while arm64 marked them
  `unemittable` and the new tests were x86-only; Task 6 lifted both.
  The arm64 JIT suite had no failure after any task that ran it
  (Tasks 2, 4, 5, 6 and 7).
- **The refresh test is five files** (`getbyid-chain-refresh.js`,
  `-refresh-demoted.js`, `-refresh-revalidate.js`, `-refresh-mapless.js`,
  `-refresh-settle.js`), one per case listed under Testing, and two of
  them are redesigned so that their claims are observable:
  - The base case runs 48 rounds of bump-and-read. One bump cannot
    tell the fast refresh from revalidation: both set the epoch and both
    count a refresh. 48 forwarded misses would demote the site, so the
    test pins `NumByIdChainRefreshes: 48` with `NumByIdDemotions: 0`
    and 14 hits; without the fast refresh it demotes.
  - Variant (c), the mapless holder, builds 48 flagged twins on the
    holder's flagged class C'. The spec's single sibling did not keep
    the holder's class mapless at the fast refresh: version 1's
    forwarded lookups steal the map into C'. In the test, each round
    adds a new property to one twin, which moves C''s map to the new
    child before the bump, so every one of the 48 fast refreshes walks
    the mapless C' and reaches p's flags transition through
    `findPropertyNoAlloc`. Reverting adee03e65's HiddenClass.cpp hunk
    demotes the site (`NumByIdDemotions: 1`).
- **The positive side of `refreshFailedAt` suppression is not observable
  by any counter.** A verdict that depends only on flagged objects
  cannot change without a bump, so a suppressed walk would have
  returned the same verdict and refreshed nothing; suppression only
  saves walks, and neither `NumByIdChainRefreshes` nor the decline
  count sees them. `getbyid-chain-refresh-settle.js` pins the negative
  side (a Rejected verdict on an unflagged path must not suppress).
  Never setting `refreshFailedAt` passes the whole JIT suite.
- **The invalidate test's `setPrototypeOf` case** moves `p` to a new
  slot, so it reads a different answer; the same-slot replacement is
  `getbyid-chain-refresh-revalidate.js`.
- **The exclude test** adds an own-property receiver, so that the
  excluded site compiles a version 2 whose dump can show `0 chain`. It
  puts `name` two prototypes up from functions with `setPrototypeOf`,
  because `Function.prototype` has its own `name` (a depth-1 hit).
- **Tests the spec did not list:** `getbyid-chain-record.js` (an entry
  that is recorded and refreshed but never emitted: the call still
  forwards and counts), `getbyid-chain-hcid.js` (with one lazy JIT ID,
  a chain entry with no ID is marked `unemittable` and is not progress)
  and `getbyid-chain-rooted.js` (the forwarded result stays rooted
  across flagging; hsan tree). `JitChainWalkTest` has three cases:
  the spec's typed holder, a flags-transitioned holder without a map,
  and the verdict table.
- **The arm64 `static_assert`** on `runtimeParentCacheEpoch` (the
  offset is 8-aligned and below `kMaxInlineBaseOffset`) held in all
  three arm64 heap modes; no decision was needed.

### Matrix

The final suites ran after Task 7's tests were added (the last code
change is Task 6's). Task 7's review fix then changed four test files
(`getbyid-chain-refresh-mapless.js`'s code; comments only in
`-refresh.js`, `-refresh-revalidate.js` and `getbyid-chain-retire.js`)
and was run with `LIT_FILTER=getbyid-chain` (14 of 14 pass) on all six
x86-64 and arm64 trees; Task 8 changes only doc/JIT.md.

| tree | configuration | result |
|---|---|---|
| `cmake-build-x86jit` | HV64 ASan+Debug, JIT=2, full `check-hermes` | 4471 pass, 6 expected failures, 162 unsupported, 0 unexpected |
| `cmake-build-x86jit-hv32` | HV32 ASan+Debug, full `check-hermes` | 4472 pass, 6 expected failures, 162 unsupported, 0 unexpected |
| `cmake-build-x86jit-boxed` | BOXED ASan+Debug, full `check-hermes` | 4472 pass, 6 expected failures, 162 unsupported, 0 unexpected |
| `cmake-build-x86jit-hsan` | MallocGC + `HERMESVM_SANITIZE_HANDLES`, JIT suite | 96 pass, 75 unsupported, 1 fail: `jit/x86-64/counters-slow-call-kinds.js`, pre-existing (dz `01a06156`: its unanchored `CHECK: 2` matches the sanitizer's seed banner); `getbyid-chain-rooted.js` passes |
| `cmake-build-arm64` | HV64 qemu-user, JIT suite | 141 pass, 31 unsupported, 0 fail |
| `cmake-build-arm64-hv32` | HV32 qemu-user, JIT suite | 141 pass, 31 unsupported, 0 fail |
| `cmake-build-arm64-boxed` | BOXED qemu-user, JIT suite | 141 pass, 31 unsupported, 0 fail |
| all three arm64 trees | `aarch64/qemu-sanity.sh`, after Task 6 | 9 passed, 0 failed each |

`HermesVMRuntimeTests` runs inside `check-hermes`; on its own, at
Task 3, 378 tests passed, including the three `JitChainWalkTest`
cases.

Every new test was shown to fail with the code it pins disabled, except
for the last row (`refreshFailedAt` suppression), whose failure is none
by design. Each mutation was then restored, the tree rebuilt and the
tests re-run green.

| task | mutation | failure |
|---|---|---|
| 1 | `incParentCacheEpoch` sets the counter to 1 instead of adding 2 on wrap | `ParentCacheEpochTest`: the array fast path epoch check passes when it must fail, and the full counter is 1, not 16777217 |
| 2 | retirement does not null the entries' class roots | 9 JIT tests abort on CodeBlock.cpp's "retired ById entry holds a class" assertion |
| 3 | A: no `desc.slot <= kMaxSlot` check | `TypedHolderSlotLimit`: slot 0x10000 Found |
| 3 | B: no `findPropertyNoAlloc` fallback when the class has no map | `FlagsTransitionHolderWithoutMap` and `Verdicts`: Rejected where Found is expected |
| 3 | C: adee03e65's HiddenClass.cpp reverted | `FlagsTransitionHolderWithoutMap`: slot 1, not 0 |
| 3 | D: dictionaries not excluded | `Verdicts`: a dictionary receiver and a dictionary on the path give Found |
| 3 | E: walk bound `depth < kMaxChainDepth` | `Verdicts`: depth 8 Rejected |
| 4 | A: fast refresh completes the call for unemitted entries too | `getbyid-chain-record.js`: `NumRecompileChecks` 0, not 1 |
| 4 | B: revalidation of a stale duplicate returns before flagging | `getbyid-chain-record.js`: `NumByIdChainRefreshes` 1, not 2 |
| 5 | A: x86 epoch compare removed | `getbyid-chain-invalidate.js`: case 1 reads `1 1`; case 3 aborts on an empty value |
| 5 | B: chain entries skipped before the ID lookup | `getbyid-chain.js`: `0 chain`, no hits, 2 recompiles, 1 demotion |
| 5 | C: walk bound `kMaxChainDepth + 1` | `getbyid-chain-depth.js`: depth 9 gets a version 2 and 336 hits |
| 5 | D: the no-room branch returns before `addByIdShape` | `getbyid-chain-mega.js`: `0 megamorphic` |
| 5 | E: dictionaries not excluded | `getbyid-chain-exclude.js`: `1 chain` |
| 5 | F: a chain entry with no ID is skipped without `unemittable` | `getbyid-chain-hcid.js`: 2 recompiles, not 1 |
| 5 | G: no `emitFirstCase` after the chain slot load | `getbyid-chain-depth.js`: `100 16 16 16 100` on HV32, `100 16 100 100 100` on BOXED (HV64 needs no decode) |
| 6 | A: arm64 epoch compare removed | `getbyid-chain-invalidate.js`: `1 1`, not `10 10` |
| 6 | B: arm64 level loop starts at 2 | every chain test with depth-2 entries; depth 2 reads 16, depth 3 crashes |
| 7 | A: no fast refresh | `-refresh.js` and `-refresh-mapless.js`: `NumByIdDemotions: 1` |
| 7 | B: demotion always flips to the plain helper | `-refresh-demoted.js` (0 refreshes, 4 hits) and `-refresh-settle.js` |
| 7 | C: revalidation does not adopt the epoch (`same->epoch = now` deleted) | `-refresh-revalidate.js` (2 refreshes, 12 hits), `-refresh-settle.js`, `-record.js` |
| 7 | D: adee03e65's HiddenClass.cpp reverted | `-refresh-mapless.js`: `NumByIdDemotions: 1` |
| 7 | E: `refreshFailedAt` set whatever `allFlagged` says | `-refresh-settle.js`: 0 refreshes, 4 hits |
| 7 | F: retirement zeroes chain epochs | `getbyid-chain-retire.js`: 5 hits, not 7 |
| 7 | G: the forwarded result is not rooted across flagging | `getbyid-chain-rooted.js` (hsan): heap well-formedness assertion |
| 7 | never set `refreshFailedAt` | none: the whole JIT suite passes (see above) |
| final fix wave | A: revalidation adopts the epoch without flagging the path (the `flagChainPath` call in `chainAfterForward`'s stale-duplicate branch deleted) | `getbyid-chain-refresh-reflag.js`, the only JIT test to fail: `50 50` for both sites, not `50 90`; 46 hits, not 26 |
| final fix wave | B: the x86-64 chain entry loads its epoch into `temp3` (the receiver's lazy ID) instead of `temp4` | `getbyid-chain-mixed.js`: same values, but `NumRecompileChecks` 5 and `NumByIdDemotions` 1, not 2 and 0 |
| final fix wave | B': the same into `temp2` (the compressed receiver class) | `getbyid-chain-mixed.js`: SEGV in emitted code (the prototype block decodes the epoch as a class pointer) |
| final fix wave | C: `!desc.flags.accessor` removed from `jitWalkChain`'s Found predicate | `getbyid-chain-invalidate.js` case 4 (no new test needed): the fast refresh returns the raw `PropertyAccessor`, and `+` hands it to `toPrimitive_RJS`, which aborts on an `llvh::cast` assertion in `getMethod` |
| final fix wave | D: `_jit_get_by_id_refresh` returns its unrooted forwarded result | `getbyid-chain-rooted.js` (hsan): heap well-formedness assertion at get2's revalidating call |

### Final fix wave

A whole-branch review and an external Codex review asked for the
following; the mutations are the "final fix wave" rows above.

- **Revalidation flags the replacement path.** Deleting the
  `flagChainPath` call in revalidation passed the whole suite, because
  no test changed the revalidated path afterwards.
  `getbyid-chain-refresh-reflag.js` replaces an intermediate prototype
  with an unflagged one (same depth and slot), lets revalidation adopt
  the epoch, then shadows the property on the new intermediate, which
  bumps the epoch only if revalidation flagged it. One site revalidates
  in `_jit_get_by_id`, a demoted one in `_jit_get_by_id_refresh`.
- **Own, prototype and chain entries at one site.**
  `getbyid-chain-mixed.js` emits all three kinds at one site on both
  backends and covers an own hit, a chain hit, a prototype hit after a
  chain ID miss, a chain epoch miss that falls through the prototype
  block and the generic tier to the helper, and (in version 3) a chain
  epoch miss that must hit a prototype entry for the same receiver
  class. A register clobbered on the epoch-miss path shows in the
  counters (the lazy ID) or crashes (the class).
- **Accessor rejection** was already pinned by
  `getbyid-chain-invalidate.js` case 4 (row C).
- **`getbyid-chain-rooted.js`** gains a demoted site, so the refresh
  helper's revalidation flags while holding a pointer result. The old
  version caught an unrooted `_jit_get_by_id` only at the recording
  call: its revalidating call was the first of ten in a loop, and the
  inline hits that followed overwrote the stale result before any GC
  saw it. Each site's revalidating read is now a single call whose
  result is used after allocating.
- Comments in `JitFunctionData.h` and the arm64 emitter, and doc/JIT.md
  (the demoted refresh helper, the fast refresh in the call sequence,
  `temp4` in chain entries, the walk's flagged check, counter
  increments, the epoch readers, line widths).

Suites after the final fix wave (two new tests; the only C++ changes
are comments):

| tree | configuration | result |
|---|---|---|
| `cmake-build-x86jit` | HV64 ASan+Debug, JIT=2, full `check-hermes` | 4473 pass, 6 expected failures, 162 unsupported, 0 unexpected |
| `cmake-build-x86jit-hv32` | HV32 ASan+Debug, JIT suite | 166 pass, 8 unsupported, 0 fail |
| `cmake-build-x86jit-boxed` | BOXED ASan+Debug, JIT suite | 166 pass, 8 unsupported, 0 fail |
| `cmake-build-x86jit-hsan` | MallocGC + `HERMESVM_SANITIZE_HANDLES`, JIT suite | 96 pass, 77 unsupported, 1 fail: `jit/x86-64/counters-slow-call-kinds.js` (dz `01a06156`, as above); `getbyid-chain-rooted.js` passes |
| `cmake-build-arm64`, `-hv32`, `-boxed` | qemu-user, JIT suite | 143 pass, 31 unsupported, 0 fail each |
| all three arm64 trees | `aarch64/qemu-sanity.sh` | 9 passed, 0 failed each |

### Known limitations

- A Rejected verdict on a path that ends in unflagged objects never
  settles: `refreshFailedAt` is set only for a verdict over a fully
  flagged path, so each miss for that receiver class keeps costing two
  walks plus the forwarded lookup, until and after every epoch bump.
  Performance only (dz `01a1726e-134a`).
- DeltaBlue's remaining 8.34M `_jit_get_by_id` calls (64 sites, 3-4
  receiver classes, 89% at depth 2 or more; see Benchmarks) are
  uninvestigated (dz `01a1726e-1360`).

### Sizes

Measured with a temporary probe built with each tree's own compile
flags; the helper offsets match the `call qword ptr [r9+192]` and
`[r9+160]` in each tree's `-Xdump-jitcode=3` output.

| mode | `JitByIdShapeEntry` | `JitByIdSiteRecord` | helper offset |
|---|---|---|---|
| HV64 before | 24 | 136 | 128 |
| HV64 after | 40 | 200 | 192 |
| HV32 before | 16 | 104 | 96 |
| HV32 after | 32 | 168 | 160 |

Each entry gains the two 8-byte epochs and the depth (the kind replaces
`isProto`); the record holds `kMaxEntries` (4) entries inline, so it
grows by 64 bytes in both modes.

### Benchmarks

Release trees at base `d2d2c226e` (a scratch worktree) and head (the
code after Task 8), configured alike (`-G Ninja -DCMAKE_BUILD_TYPE=Release
-DHERMESVM_HEAP_HV_MODE=HEAP_HV_64 -DCMAKE_C_COMPILER=clang
-DCMAKE_CXX_COMPILER=clang++ -DHERMES_ENABLE_DEBUGGER=OFF`, clang 18):
`cmake-build-jit2` with `-DHERMESVM_ALLOW_JIT=2` for the JIT and
`cmake-build-release` with `-DHERMESVM_ALLOW_JIT=1` for the
interpreter. Each tree's own `hermesc -O -emit-binary` compiled each
Octane benchmark. Host idle, nothing else building or testing:

```
for rep in 1 2 3 4 5; do
  for mode in -Xjit -Xjit=off; do   # jit2 trees, release trees
    for b in richards deltablue crypto raytrace earley-boyer regexp \
        splay navier-stokes pdfjs mandreel gbemu box2d zlib typescript; do
      # base first on odd reps, head first on even reps
      taskset -c 2 <tree>/bin/hermes $mode <build>/$b.hbc
    done
  done
done
```

Octane scores, higher is better, median [min-max] of 5; "noise" means
the ranges overlap.

JIT (`cmake-build-jit2`, `-Xjit`):

| benchmark | base median [min-max] | head median [min-max] | head/base | verdict |
|---|---|---|---|---|
| Richards | 7699 [7653-7971] | 8035 [7382-8134] | 1.044 | noise |
| DeltaBlue | 4946 [4919-5025] | 7451 [7372-7557] | 1.506 | faster |
| Crypto | 7168 [7158-7171] | 7152 [7127-7171] | 0.998 | noise |
| RayTrace | 12885 [12690-13085] | 13024 [12677-13085] | 1.011 | noise |
| Earley-Boyer | 13352 [13083-13634] | 13466 [13130-13625] | 1.009 | noise |
| RegExp | 935 [924-951] | 940 [934-944] | 1.005 | noise |
| Splay | 4709 [4676-4733] | 4751 [4722-4828] | 1.009 | noise |
| NavierStokes | 14514 [14471-14648] | 14500 [14456-14633] | 0.999 | noise |
| PdfJS | 19951 [19584-20149] | 20149 [19951-20327] | 1.010 | noise |
| Mandreel | 4814 [4811-4832] | 4802 [4793-4818] | 0.998 | noise |
| Gameboy | 20518 [20222-20781] | 20618 [20438-20926] | 1.005 | noise |
| Box2D | 23644 [23551-23904] | 24015 [23268-24350] | 1.016 | noise |
| zlib | 6414 [6348-6435] | 6413 [6366-6428] | 1.000 | noise |
| Typescript | 51559 [51166-51643] | 51789 [51289-51916] | 1.004 | noise |
| geomean | | | 1.037 | |

Interpreter (`cmake-build-release`, `-Xjit=off`):

| benchmark | base median [min-max] | head median [min-max] | head/base | verdict |
|---|---|---|---|---|
| Richards | 2662 [2559-2677] | 2641 [2485-2651] | 0.992 | noise |
| DeltaBlue | 2460 [2448-2477] | 2431 [2327-2444] | 0.988 | slower |
| Crypto | 3043 [2826-3071] | 3070 [3018-3089] | 1.009 | noise |
| RayTrace | 7739 [7599-7782] | 7716 [7665-7770] | 0.997 | noise |
| Earley-Boyer | 9376 [9340-9408] | 9290 [9074-9476] | 0.991 | noise |
| RegExp | 962 [953-965] | 964 [953-971] | 1.002 | noise |
| Splay | 4502 [4485-4559] | 4490 [4458-4563] | 0.997 | noise |
| NavierStokes | 5137 [4923-5362] | 5072 [4918-5227] | 0.987 | noise |
| PdfJS | 14788 [14716-14904] | 14674 [14617-14803] | 0.992 | noise |
| Mandreel | 2381 [2333-2403] | 2386 [2369-2405] | 1.002 | noise |
| Gameboy | 14866 [14824-14922] | 14838 [14727-14937] | 0.998 | noise |
| Box2D | 12458 [12396-12525] | 12384 [12274-12483] | 0.994 | noise |
| zlib | 4166 [4138-4183] | 4138 [4136-4156] | 0.993 | noise |
| Typescript | 35039 [34962-35252] | 34933 [33839-35193] | 0.997 | noise |
| geomean | | | 0.996 | |

Against the expectation:

- DeltaBlue's JIT score rose 50.6% (4946 to 7451), far beyond noise.
- DeltaBlue's slow GetById calls dropped by more than 90%: met on the
  fixed-work run below (7.4% of base, a 92.6% drop). The shipped
  time-based totals (16.3%) are not comparable, because head executes
  about 2.2x more iterations in the same time.
- No JIT benchmark other than DeltaBlue moved beyond noise in either
  direction; the JIT geomean is 1.037, almost all of it DeltaBlue's.
- Interpreter DeltaBlue missed "unchanged within noise" by 4 score
  points: base min 2448, head max 2444 (0.988). Ten more alternating
  pairs gave medians 2471.5 and 2448.5 (0.991) with overlapping ranges
  (base 2404-2497, head 2387-2468). A `perf record -F 2000` of one run each
  shows the same profile: `interpretFunction` 75.6% and 74.7%,
  `getNamedDescriptorUnsafe` 8.9% and 8.2%, `doGetByIdSlowPath_RJS`
  5.9% and 6.7%, `getNamedWithReceiver_RJS` 3.2% and 3.8%, and no new
  symbol. The only change the interpreter executes is the 64-bit epoch:
  a mask in `getParentCacheEpoch`, which every add-cache lookup calls,
  a wider increment, and the shift it causes in `Runtime`'s layout. The
  loss is about 1%, at the edge of noise, and unexplained. The other
  interpreter scores are noise; the geomean is 0.996.

JIT counters (`-Xjit-emit-counters`, one DeltaBlue run each): base
18 recompiles, 51 GetById demotions; head 42 recompiles, 31 GetById
demotions, 66.3M `NumByIdChainHits`, 0 `NumByIdChainRefreshes` (the
instrumented runs below saw the same epoch at every slow call).

DeltaBlue's slow GetById calls, counted with the dz 970b
instrumentation (temporary counters in `getByIdWithReceiver_RJS`,
not committed), `cmake-build-jit2`, `-Xjit`:

| run | base | head | head/base |
|---|---|---|---|
| deltablue.hbc as shipped (runs for a fixed time) | 31,449,520 | 5,136,668 | 16.3% |
| `doDeterministic: true` (4400 iterations per phase, fixed work) | 285,368,590 | 21,127,328 | 7.4% |

The shipped benchmark runs each phase for one second, so the faster
head does more iterations (the instrumented binaries scored 3213 and
7081) and its raw total is not comparable; the fixed-work run is, and
it gave the same total on two runs of each binary. Of head's 21.1M,
12.76M are `prototype` reads from `_sh_ljs_create_this` (`new`), which
are not GetById sites and equal base's count exactly; 8.34M go through
`_jit_get_by_id` from 64 sites that still record (99% of these calls
come from sites that saw 3 or 4 receiver classes, and 89% find the
property at depth 2 or more), and 36K through the plain helper; why
those sites keep missing was not investigated (dz `01a1726e-1360`).
Through the GetById helpers alone, base made 272.6M calls and head
8.4M (3.1%).

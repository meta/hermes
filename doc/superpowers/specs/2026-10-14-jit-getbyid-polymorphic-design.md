# JIT: polymorphic GetById tier with per-site shape feedback — design

Date: 2026-10-14. Status: reviewed (Codex, 4 rounds: 2 blockers and
7 major issues found and resolved); implemented on x86-64, see
"Delivered" at the end.
Scope: x86-64 JIT only. arm64 keeps calling the plain helper and gets
the tier in a later parity task, as GetByVal and PutByVal did.
Companions and templates:
doc/superpowers/specs/2026-09-11-jit-getbyval-design.md and
doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md.
This design reuses their version records, recompilation trigger,
helper-slot indirection and demotion rule wherever GetById does not
genuinely differ.

## Motivation

Benchmark selection and baseline profiles are recorded in dz
`01a13c62`. After commit 4f0c853fd removed the `== null` hotspot,
Octane Richards spends ~89% of its time in JIT code; the remaining
~9% in the runtime is getById misses (`_sh_ljs_get_by_id_rjs`,
`getNamedDescriptorUnsafe`, `getNamedWithReceiver_RJS`,
`HiddenClass::findProperty`). A temporary histogram in the helper
attributed effectively all of them to ONE site: `this.task.run(packet)`
in `TaskControlBlock.prototype.run` — 22.9M misses per run, 3 distinct
receiver hidden classes, `run` found on 4 different task prototypes.

The site misses on every execution because:

- `GetByIdImpl::emitFastPath` (x86-64 JitEmitter-property.cpp) emits
  an object or parent specialization only when the read cache had
  exactly one good change (`numGoodChanges == 1`). A polymorphic cache
  gets only the generic tier.
- The generic tier compares against the LIVE cache entry and handles
  own properties only; a prototype hit is inline only through the
  single-shape parent specialization.
- The interpreter's one-entry cache thrashes between the shapes, so
  the helper does a full lookup almost every time.
- `_sh_ljs_get_by_id_rjs` carries no version record, so GetById
  declines cannot trigger a recompile and nothing records the shapes a
  site sees (doc/JIT.md, "Recompilation": "declines from GetById alone
  do not trigger").

The same pattern — polymorphic and prototype reads falling into
`getNamedDescriptorUnsafe` — is the top runtime cost in DeltaBlue
(~31% of cycles in getById misses), RayTrace and Earley-Boyer.

## Policy (settled in design discussion, 2026-10-14)

- **Evidence-driven, monotone tiers.** A recompile adds a polymorphic
  inline chain at a site whose record holds shapes; shapes are never
  dropped from later versions.
- **No deopt, no speculation.** Every inline check falls through to
  the generic tier and then to the helper; versions stay semantically
  interchangeable.
- **Version 1 is unchanged** apart from the helper call going through
  the site's helper slot and the recording helper.
- **Weak records, IDs at emission** (approach A). Records hold weak
  hidden-class references; lazy JIT IDs, which pin a class forever,
  are assigned only when a recompile emits the entry. Rejected:
  recording IDs at observation time (pins classes nobody emits, drains
  the 16-bit ID space) and a polymorphic interpreter read cache
  (changes the interpreter hot path and every site's memory).
- **Capped at K = 4 entries per site.** A fifth distinct shape marks
  the site megamorphic; the first four are kept, as PutByVal keeps its
  first typed-array kind.
- **Validity rules are the read cache's own.** The recorder harvests
  what the interpreter's cache fill just wrote, so data-property-only,
  immediate-prototype-only and slot range come for free, and it
  re-checks the cache's class predicates itself (see "Eligibility"
  below). Today's single-shape tiers obey the same predicates, so
  every shape they emit is recordable and monotonicity needs no
  special case.

## Records

`JitConsumerRecords` (include/hermes/VM/JIT/JitFunctionData.h) gains a
second deque, next to `byValSites`:

```cpp
/// One observed GetById shape: a receiver class, and for a prototype
/// hit the immediate prototype's class, plus the slot to load.
struct JitByIdShapeEntry {
  /// The receiver's hidden class.
  WeakRoot<HiddenClass> objClass{nullptr};
  /// The immediate prototype's hidden class; used only when isProto.
  WeakRoot<HiddenClass> protoClass{nullptr};
  /// Slot in the receiver (own entry) or in the prototype.
  uint16_t slot = 0;
  /// Set when the entry is recorded and never changed: a prototype
  /// entry whose protoClass the GC cleared is dead, never an own entry.
  uint8_t isProto = 0;
  /// The owning body emitted an inline check for this entry. Carried
  /// forward: an emitted entry's classes are pinned by their lazy JIT
  /// IDs and every later version must emit it again.
  uint8_t emitted = 0;
  /// A compile could not emit this entry (no lazy JIT ID available).
  /// Never progress. Reset on carry-forward.
  uint8_t unemittable = 0;
};

struct JitByIdSiteRecord {
  static constexpr unsigned kMaxEntries = 4;
  /// Bytecode offset of the GetById instruction.
  uint32_t siteId;
  llvh::SmallVector<JitByIdShapeEntry, kMaxEntries> entries;
  /// The recording helper has completed at least one call here,
  /// cacheable or not. Plays the role of ByVal's otherSeen/jsArraySeen
  /// in the demotion rule's "observed" test.
  uint8_t seen = 0;
  /// A distinct live shape arrived while entries was full of live ones.
  uint8_t megamorphic = 0;
  /// Same meaning as in JitByValSiteRecord.
  uint8_t changed = 0;
  uint8_t unchangedCrossings = 0;
  /// Per-site mutable helper slot: _jit_get_by_id while recording,
  /// _sh_ljs_get_by_id_rjs after demotion. Never flipped back.
  void *helper = nullptr;
};
```

`byIdSites` is a `std::deque` for the same reason `byValSites` is:
emitted code embeds the address of a record's `helper` slot, so
records never relocate. Lookup is a linear scan via
`findOrCreateByIdSite(siteId)`. The emitter creates the record when it
emits the site's helper call, so the recording helper never grows the
deque.

An entry is **live** when `objClass` is non-null and, if `isProto`,
`protoClass` is non-null. Entry identity (for dedup) is
`(isProto, objClass, protoClass if isProto, slot)`. Only live entries
are ever emitted or count as progress.

`recordingByValSites` is generalized to `recordingSites`, counting
still-recording ByVal AND ById sites, so the demotion pass's
constant-time gate covers both.

### Eligibility

The predicates are exactly the read cache fill's
(lib/VM/JSObject.cpp ~1155, lib/VM/StaticH.cpp ~1193), re-checked by
the recorder:

- the class holding the property — the receiver for an own entry, the
  prototype for a prototype entry — is not `isDictionaryNoCache()`;
- for a prototype entry, the receiver class is not `isDictionary()`
  (negative caching: a dictionary can gain properties without changing
  class).

Why a cacheable dictionary is safe as a property holder: while it
remains the object's class, an existing data property keeps its slot
and its data interpretation. Deleting from it, and flag changes
through `updateProperty`/`defineOwnProperty`, replace the class
(`copyToNewDictionary`, HiddenClass.cpp ~378 and ~547) before more JS
runs, so emitted code keyed to the old class ID stops matching.
Additions may reuse a deleted slot (DictPropertyMap.cpp ~233) but do
not disturb an existing property. `updatePropertyFlagsWithoutTransitions`
mutates a dictionary in place, but its callers only clear
writable/configurable or set staticBuiltin, which change neither the
slot nor the data/accessor kind. Today's single-shape tiers rely on
the same facts.

## The recording helper

New handler in lib/VM/JIT/JitHandlers.{h,cpp}:

```cpp
SHLegacyValue _jit_get_by_id(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *cacheEntry,
    SHJitVersionData *versionData,
    uint32_t siteId);
```

The order matches the ByVal recording helpers: the observation that
caused a threshold crossing is recorded before the crossing is
processed.

1. **Forward.** Call `_sh_ljs_get_by_id_rjs(shr, source, symID,
   cacheEntry)` and store the result immediately in a `PinnedValue`
   of a `Locals` struct. Semantics, exceptions and the cache fill are
   exactly today's. The cache entry is the one associated with this
   instruction; cache indices can be shared by property name
   (lib/BCGen/HBC/ISel.cpp ~2695), which the harvesting predicates
   below tolerate — they never assume exclusive ownership. If the
   forwarded call throws, it unwinds past the helper: nothing is
   recorded and nothing is counted. A site that only ever throws
   therefore never demotes; it pays only the helper indirection,
   because it never reaches the recording or counting steps.
2. **Staleness.** Look up the site record. Record only if `vd` is
   still the function's `current` version and `site.helper` is still
   `_jit_get_by_id`. A getter run by step 1 can recompile and retire
   this version, or a demotion pass can flip the site; either way the
   observation is dropped. (Retirement clears entries but keeps the
   record, so holding the record across step 1 is safe.)
3. **Record** (inside a `NoAllocScope`; nothing here can collect).
   Set `seen` (marking `changed` the first time). Read the object
   from `*source` — a frame register the GC updates — and its current
   class `now`; nothing from before step 1 is reused, since a GC
   during it can move both. If the source is not an object, stop
   harvesting and go on to step 4.
   Classify the cache entry, comparing its weak roots without a
   barrier:
   - **own hit:** `cacheEntry->clazz == now`, regardless of
     `negMatchClazz` (the own-property fill does not clear it; this is
     the lookup's own precedence, StaticH.cpp ~1158) → shape
     `(own, now, slot)`;
   - **prototype hit:** otherwise, `cacheEntry->negMatchClazz == now`,
     the object's parent is non-null, and `cacheEntry->clazz` equals
     the parent's class → shape `(proto, now, parentClass, slot)`;
   - anything else (uncacheable miss, accessor, deeper prototype, an
     entry describing another receiver): no shape.

   Apply the Eligibility predicates. Then: if an equal live entry
   exists, nothing changes. Else if a dead entry exists, overwrite it
   entirely with the new shape (all fields, including `emitted` and
   `unemittable`, reset). Else if fewer than K entries, append. Else
   set `megamorphic`. Overwrite and append set `changed`; setting
   `megamorphic` sets `changed` only on its 0 → 1 transition (as ByVal
   marks `taPoisoned` once), so sustained overflow traffic does not
   keep resetting demotion stability.
4. **Count.** `++vd->declineCount`; at `declineThreshold` call
   `JITContext::considerRecompile(runtime, vd)`. This can compile and
   collect; the result is rooted by step 1.
5. Return the pinned result.

`_jit_get_by_id` is `LLVM_ATTRIBUTE_NOINLINE` and is called with the
saved-IP indirect form, because the fallback can run getters and
throw.

## Emission (x86-64)

Applies to `GetById`, `GetByIdShort` and `GetByIdLong` — all reach
`Emitter::getById` → `getByIdImpl` with `_sh_ljs_get_by_id_rjs`.
`TryGetById*` (globals) and `GetByIdWithReceiver*` keep today's code.
`getByIdImpl` gets a flag selecting the recording form; only the
GetById family sets it.

**Helper call.** For the GetById family the slow-path call becomes:
rdi = runtime, rsi = &source, edx = symID, rcx = cache entry,
r8 = versionData, r9d = siteId, then the indirect call through
`site.helper` (initialized to `_jit_get_by_id` if null). A demoted slot
holds the 4-argument `_sh_ljs_get_by_id_rjs`, which ignores r8/r9
under SysV, as the ByVal slots rely on for their own signatures. Sites
with `cacheIdx == PROPERTY_CACHING_DISABLED` keep the direct call: with
no cache entry there is nothing to harvest.

**Records per site.** The emitter creates the site's record (via
`findOrCreateByIdSite`) when it emits the recording helper call, i.e.
for every GetById-family site with a cache entry in every compiled
body. Cost: one deque element per such site, in compiled functions
only; `JitByIdSiteRecord` embeds K entries of inline `SmallVector`
storage, so its size is configuration-dependent. The implementation
reports `sizeof(JitByIdSiteRecord)` for HV64 and HV32 in the delivered
section, and the deque's own block overhead separately.

**Tier selection.** Using the candidate's record for the site:

- **No live entry:** today's selection, unchanged — object
  specialization, else parent specialization, else generic — with
  today's code shape, including the parent specialization being
  terminal. Whatever specialization is emitted is also written into
  the record as an `emitted = 1` entry (it satisfies Eligibility by
  construction, since it came from the same cache fill), so a later
  version keeps it even though its hits never reach the helper.
- **At least one live entry:** emit the **polymorphic chain** in place
  of the single-shape choice. If the interpreter cache currently
  satisfies today's single-shape gate and that shape is eligible and
  not already in the record, it is added as an entry first, subject to
  the cap.

Lazy IDs are assigned for every candidate entry's classes, through
`WeakRoot::get(runtime, heap)` (the read barrier matters during
concurrent marking), before any chain code is emitted:
`initHCLazyIDMayAlloc` is a GC safepoint, and an entry can die across
it (see GC). An entry that is dead or cannot get an ID is skipped and
marked `unemittable`; every emitted entry is marked `emitted`. If no
entry survives, fall back to the no-live-entry selection.

**Chain shape and register state.** Own entries first, then
prototype entries. Temporaries are GetByIdImpl's `temp1..temp4`;
`res` is the result register.

```
  is-object(src) else -> slow call                ; NOT the generic tier
  temp1 = object pointer
  temp2 = obj->clazz (compressed)                 ; kept for the generic tier
  temp3 = lazyJITId(decode(temp2))                ; decode preserving temp2,
                                                  ; as emitObjectSpecialization does
  ; own entries
  cmp temp3, ID_own1 ; jne n1
  res = load slot1 from temp1 ; decode.firstCase ; jmp done
n1: ...
  ; prototype block, only if some prototype entry was emitted;
  ; temp3 holds the receiver ID here even with zero own entries
  temp4 = decode(temp2)->objectParent ; test ; jz generic_join
  temp2 = lazyJITId(temp4->clazz)                 ; temp2 now scratch
  cmp temp3, ID_objA ; jne p1 ; cmp temp2, ID_protoA ; jne p1
  res = load slotA from temp4 ; decode.firstCase ; jmp done
p1: ...
generic_join:
  temp2 = obj->clazz (compressed), reloaded from temp1, only if the
          prototype block was emitted
  ; join state: temp1 = object, temp2 = compressed receiver class
  today's generic live-cache tier, unchanged
  slow call through the helper slot
done:
```

An own-entry load may clobber `temp1` (indirect slots) only on its hit
path, which jumps to `done`. A prototype-entry load clobbers `temp4`
only after both of its guards succeeded.

**One decoder per site.** A single `Emit_sh_shv_decode` serves the
whole chain and the generic tier: every chain load and the generic
tier's direct-slot load use `emitFirstCase` followed by `jmp done`,
and the rest cases are emitted exactly once — by the generic tier's
indirect-slot `emitAll` as today — with their final fallthrough
reaching `done`, never the slow call (JitEmitter-internal.h ~584).
Chain prototype entries therefore do not use `emitAll`, unlike today's
parent specialization.

Unlike today's parent specialization, the chain is always followed by
the generic tier.

Size: with K = 4, roughly 100–150 bytes per site, only at sites with
live entries.

## Recompilation, demotion, carry-forward

**Progress term.** `considerRecompile` gains `byIdShapeProgress(vd)`:
some ById site record holds a live entry with `emitted == 0` and
`unemittable == 0`. It is ORed with the two existing terms
(cold-ById-warmed, ByVal shape). Budget (`-Xjit-max-recompiles`,
default 2) and threshold (`-Xjit-recompile-threshold`, default 64) are
unchanged.

**Demotion.** `isRecordingHelper` recognizes `_jit_get_by_id`;
`demoteSite` for a ById record flips it to `_sh_ljs_get_by_id_rjs`.
The existing pass walks `byIdSites` with the same rule as ByVal:

- skip a site whose `seen` is clear (its helper never completed a
  call; flipping it would silence a site that might still learn);
- reset `unchangedCrossings` if the site can still progress (a live
  unemitted, emittable entry exists) and a recompile is possible
  (budget left and JIT enabled), or if `changed`;
- else count up and flip at `kDemotionStableCrossings` (3), counting
  `NumByIdDemotions`.

A megamorphic site whose four entries are emitted, and a site whose
traffic is entirely uncacheable (missing properties, getters,
primitive receivers, ineligible dictionaries), have nothing left to
learn and demote — on the existing schedule: the first crossing after
`seen` is set is a "changed" crossing, then three stable crossings.

**Carry-forward.** The `Compiler` constructor copies `byIdSites` from
`current` like `byValSites`: entries (classes, slot, `isProto`,
`emitted`), `seen`, `megamorphic` and the helper slot value carry;
`changed` and `unchangedCrossings` reset; `unemittable` resets (the ID
space or a class's liveness may differ next time). `emitted` must
carry: it is what makes emission monotone.

**Retirement.** `demoteAllSitesOnRetirement` also flips the retired
body's still-recording ById slots, and clears every ById record's
entries — a retired body's observations are never read, so its weak
references need not be traced.

## GC

Records hold `WeakRoot<HiddenClass>`, written the way the cache fill
writes `ReadPropertyCacheEntry::clazz` (assignment from
`getClassGCPtr()`), and read in the emitter with
`WeakRoot::get(runtime, heap)`; raw `getNoBarrierUnsafe()` only for
null and equality tests.

`CodeBlock::markWeakElementsInCaches` additionally visits, with
`acceptor.acceptWeak`, both classes of every entry in:

- `jitData_->current`'s `byIdSites`;
- a new `JitFunctionData::compiling` (raw `JitVersionData *`), set by
  the `Compiler` constructor to the candidate after carry-forward and
  cleared on install and on every failure path (the `Compiler` is
  destroyed normally on the longjmp path; clearing goes in its
  destructor). Without it, a GC during emission — `initHCLazyIDMayAlloc`
  allocates — would not trace the candidate's copied weak references.
  This applies to the FIRST compile too: today `JitFunctionData` is
  first created at install (JitCompiler.cpp ~549), so the constructor
  must call `ensureJitData` itself to publish the candidate; a first
  compile's candidate records the legacy specializations it emits.

Retired records are cleared at retirement, so they need no visiting;
the recorder's staleness check (helper step 2) keeps a retired record
from being repopulated by an in-flight helper call. Emitted entries'
classes are pinned through `usedHCs` and never die.

The hook is sufficient for every collector: hidden classes are
allocated `LongLived::Yes` (HiddenClass.cpp ~222), directly in Hades's
old generation, so young collections, which skip long-lived weak roots,
never move them; old-generation collections and MallocGC visit
long-lived weak roots, and weak clearing happens in the stop-the-world
completion phase.

## Counters and dump output

- `NumByIdDemotions` joins `JIT_COUNTERS`, documented like
  `NumByValDemotions` (retirement flips not counted).
- The `-Xdump-jitcode` per-function summary in
  `compileCodeBlockImpl` gains
  `JIT ById sites: <recorded> recorded, <poly> polymorphic, <mega> megamorphic`,
  printed when `recorded > 0`, where `recorded` counts sites with at
  least one live entry or the megamorphic flag and `poly` counts sites
  whose body emitted a chain.
- The emitted code is commented `// Get polymorphic chain (N entries)`
  so dumps and perf jitdump attribution see it.

## Non-goals

- arm64 (later parity task).
- Accessor properties (getters) and properties deeper than the
  immediate prototype — the read cache does not cache them.
- `TryGetById*` and `GetByIdWithReceiver*`.
- Polymorphic PutById. The add-property put path is a separate target
  (dz `01a13c62`, lever 2).
- Reclaiming retired bodies (existing dz `01a06d06-a7d7`).

## Testing

New lit tests in test/jit/x86-64/, `REQUIRES: jit-arch-x86_64`, run
with `-Xjit`, a lowered `-Xjit-recompile-threshold`,
`-Xjit-crash-on-error`, and checking program output plus the
`JIT ById sites` dump line and counters:

- polymorphic own properties with 2, 3 and 4 receiver shapes;
- prototype methods, Richards-style: one receiver class reaching two
  prototypes, and two receiver classes reaching one prototype;
- own and prototype entries mixed at one site;
- megamorphic: six shapes — four emitted, the rest correct through the
  helper; overflow shapes keep arriving through the stable crossings
  and the site still demotes (`NumByIdDemotions`);
- invalidation without deopt, all producing correct values through
  misses: a later own property shadowing a prototype hit; prototype
  swapped with `Object.setPrototypeOf`; prototype turned into a
  dictionary by `delete`; a data property redefined as a getter;
- monotonicity: a version-1 single-shape specialization survives into
  version 2 after the site turns polymorphic;
- GC: shapes recorded, receivers dropped, `gc()` forced, then a
  recompile — dead entries skipped, no crash; and a compile that
  collects during emission (allocation pressure with
  `-Xjit-hc-id-limit` / forced GC), run on a MallocGC +
  `HERMESVM_SANITIZE_HANDLES` build in the matrix with
  `-gc-sanitize-handles=1` (the build alone moves the heap at only 1%
  of allocations; the flag makes it move every live object at every
  allocation);
- demotion and retirement: end-state `NumByIdDemotions` pins and the
  A/B crossing-equality technique of the ByVal demotion tests;
- ID exhaustion: tiny `-Xjit-hc-id-limit` — entries unemittable, no
  recompile loop, correct results;
- prototype-class death: a prototype entry whose prototype class dies
  while the receiver class survives (prototype swapped, old prototype
  collected), then a recompile — the entry is dead, never emitted as
  an own load, values correct;
- own shapes after prototype traffic: prototype hits first, then
  alternating own shapes at the same site — own entries are recorded
  (the stale `negMatchClazz` does not block them);
- uncacheable-only site (missing property / getter / primitive
  receiver): `seen` drives demotion on the documented schedule;
- threshold boundary: the first cacheable shape arrives on the very
  call that crosses the threshold on which the site would otherwise
  demote — the shape is recorded and the site does not demote;
- retirement during a helper call: a getter reached through the
  recording helper recompiles its own function; the retired record is
  not repopulated and the run is correct;
- rooted result: a pointer-valued result (object and string, plus a
  boxed double under HEAP_HV_BOXED) on the call that crosses the
  threshold and triggers a compile that collects;
- generic join: after a chain is installed, an unrecorded own shape
  that hits the live cache still loads inline; primitive receivers go
  straight to the helper; pointer and boxed-double values through
  chain own entries, chain prototype entries and the generic tier
  (exercises the single shared decoder tail);
- cacheable-dictionary receiver: an own entry on a cacheable
  dictionary survives into the chain; deleting a property afterwards
  (new class) misses correctly.

Full `check-hermes` on the ASan+Debug build, and the JIT matrix of
doc/JITTesting.md (HV32, boxed doubles, MallocGC).

## Benchmarks and expected numbers

Method as in dz `01a13c62`: precompiled bytecode, `taskset -c 2`,
median of 5, A/B against the parent commit with the same compiler.
Richards, DeltaBlue, RayTrace, TypeScript, Earley-Boyer. Plus a check
with a perf-prof build (`HERMES_ENABLE_PERF_PROF`) that the Richards
`run` site no longer reaches `_sh_ljs_get_by_id_rjs`.

Expected: Richards +8–10%; DeltaBlue and RayTrace measurably up;
nothing worse than noise.

## Docs

doc/JIT.md: "Inline caches" (the chain) and "Recompilation" (GetById
now triggers and records). A "Delivered" section appended here with
the measured numbers.

## Delivered (2026-10-14)

Commits `01e305d36..c99aaead8` on branch `x86-jit` (base `84aee64ba`).

### Differences from the text above

- `JitByIdSiteRecord` has one more byte, `chainEmitted`: the body
  emitted a chain at this site. It is reset on carry-forward and feeds
  only the `polymorphic` count of the dump line, which therefore
  includes one-entry chains carried forward from a single-shape
  specialization.
- Step 1 of the recording helper registers its `Locals` only after
  the forwarded call returns. A throwing `_sh_ljs_get_by_id_rjs`
  longjmps past the helper and would skip `~LocalsRAII`, leaving
  `runtime.vmLocals` pointing at a dead frame (found under ASan;
  `DESIGN-PRINCIPLES.md` now states the rule). Nothing allocates
  between the return and the pin.
- Because the `Compiler` constructor now creates `JitFunctionData`
  before the first install, the version number is derived from
  whether a body is installed (`current`), not from the data's
  existence.
- `GetByIdImpl` records the legacy specializations as `emitted`
  entries after emitting them; the chain path replaces them only when
  the record already holds a live entry.
- Storage (final-review fix): `byValSites` and `byIdSites` are
  `std::vector<std::unique_ptr<...>>`, not `std::deque`. Each record
  is its own allocation, so addresses stay stable, and an empty vector
  allocates nothing. The carry-forward deep-copies each record.
- Helper argument (final-review fix): the last argument of
  `_jit_get_by_id` is the site's record (`SHJitByIdSiteRecord *`), not
  `uint32_t siteId`. The emitter loads it into `r9` and calls through
  `[r9 + offset of helper]`, so the helper does no lookup and
  `findByIdSite` is gone. The staleness gate is unchanged. The pointer
  stays valid for an in-flight call whose body is retired, because the
  retired `JitVersionData` keeps its records and is never freed.

### Matrix

All at `c99aaead8`, clang 18, each tree rebuilt before its run.

| tree | configuration | result |
|---|---|---|
| `cmake-build-x86jit` | HV64 ASan+Debug, full `check-hermes` | 4272 pass, 6 expected failures, 157 unsupported, 0 unexpected |
| `cmake-build-x86jit-hv32` | HV32 ASan+Debug, JIT suite | 148 pass, 7 unsupported |
| `cmake-build-x86jit-boxed` | BOXED ASan+Debug, JIT suite | 148 pass, 7 unsupported |
| `cmake-build-x86jit-rel` | Release, JIT suite | 148 pass, 7 unsupported |
| `cmake-build-x86jit-malloc` | MallocGC ASan+Debug, JIT suite | 129 pass, 26 unsupported |
| `cmake-build-x86jit-hsan` | MallocGC + `HERMESVM_SANITIZE_HANDLES` ASan+Debug, JIT suite | 94 pass, 61 unsupported |

The hsan run had no failure, but only by chance. The pre-existing
`jit/x86-64/counters-slow-call-kinds.js` failure (dz `01a06156`) is
seed-dependent: the sanitizer's random-sampling banner goes to stderr,
which the test pipes into FileCheck, and the test's unanchored
`CHECK: 2` matches inside the banner whenever the random seed contains
the digit 2. Pinned with `-gc-sanitize-handles-random-seed`, the base
(`84aee64ba`, same configuration in a separate worktree) and head
binaries behave identically: seeds 4081612497 and 2000000000 fail,
1111111111 and 3456789013 pass. In 40 unpinned runs each, base failed
32 and head 28, and every failure had a seed containing a 2.

arm64 (verified later, at 18d95ea7d, after the cross toolchain and qemu
were installed per aarch64/README.md): cmake-build-arm64,
cmake-build-arm64-hv32 and cmake-build-arm64-boxed all build; each
passes aarch64/qemu-sanity.sh (9/9) and the JIT suite (105 pass, 50
unsupported: the 49 x86-64-only tests and large_literal_obj.js),
including count.js, recompile-phased-warming.js and
recompile-getbyid-cold.js. The arm64 emitter itself still has no
polymorphic tier; that is dz `01a13d77`.

### Sizes

Re-measured after the storage change, with a temporary probe built with
each tree's own compile flags (minus ASan) and a counting allocator;
the helper offsets it printed (128 on HV64, 96 on HV32) match the
`call qword ptr [r9+128]` and `[r9+96]` in each tree's
`-Xdump-jitcode=3` output.

| mode | `JitByIdShapeEntry` | `JitByIdSiteRecord` | `JitConsumerRecords` |
|---|---|---|---|
| HV64 | 24 | 136 | 56 |
| HV32 | 16 | 104 | 56 |

`JitConsumerRecords` was 168 B with the two deques. Heap held by
`byIdSites` alone, requested bytes and glibc chunk bytes:

| sites | HV64 requested | HV64 chunks | HV32 requested | HV32 chunks |
|---|---|---|---|---|
| 0 | 0 | 0 | 0 | 0 |
| 1 | 144 | 176 | 112 | 144 |
| 8 | 1152 | 1232 | 896 | 976 |
| 64 | 9216 | 9744 | 7168 | 7696 |

Per site: `sizeof(JitByIdSiteRecord)` plus one 8-byte vector slot
(144 B requested on HV64, 112 B on HV32), plus glibc's 8-byte chunk
header and rounding (about 152 B and 120 B per site at 8 or more
sites). Vector growth by doubling can leave up to twice the slots
in use. A `JitByValSiteRecord` is 24 B, so a ByVal site costs 32 B
requested.

Memory floor. An empty container allocates nothing, so a compiled
function with no ByVal or GetById record has no `JitConsumerRecords`
at all, as before this feature. A function with one cached GetById
site carries `JitConsumerRecords` (56 B) and that site: 200 B
requested on HV64 and 168 B on HV32. With the deques, the same
function carried at least 1208 B on HV64 and 1216 B on HV32, because
libstdc++ allocates a map and a block even for an empty deque.

Site lookup. `_jit_get_by_id` receives its record's address, so a
decline does no lookup. The emitter's `findOrCreateByIdSite` and the
ByVal recording helpers' `findOrCreateByValSite` remain linear scans:
the first runs once per site per compile, the second on every ByVal
decline until the site demotes, as before this feature.

### Benchmarks

Release JIT trees (`-DCMAKE_BUILD_TYPE=Release -DHERMESVM_ALLOW_JIT=1`,
clang 18, no perf-prof) built at base `84aee64ba` (separate worktree)
and head `c99aaead8`. Each tree's own `hermesc -O -emit-binary`
compiled each benchmark. Host idle, nothing else building or testing,
`taskset -c 2`, 5 runs, alternating base/head:

```
for b in richards deltablue raytrace typescript earley-boyer; do
  for i in 1 2 3 4 5; do
    taskset -c 2 <base>/bin/hermes -Xjit base-$b.hbc
    taskset -c 2 <head>/bin/hermes -Xjit head-$b.hbc
  done
done
```

Octane scores, higher is better, median [min-max]:

| benchmark | base | head | head/base |
|---|---|---|---|
| Richards | 6524 [6231-7255] | 7293 [6231-7413] | 1.118 |
| DeltaBlue | 5012 [4985-5032] | 5065 [4939-5091] | 1.011 |
| RayTrace | 11174 [10919-11362] | 12677 [12580-13098] | 1.135 |
| TypeScript | 51331 [51269-51705] | 51289 [50497-51414] | 0.999 |
| Earley-Boyer | 14011 [13870-14120] | 13818 [13515-13908] | 0.986 |

Against the expectation: Richards (+12%) and RayTrace (+13.5%) meet
or beat it. The Richards median is weak evidence: its samples are
bimodal and both sides share the minimum, 6231. The perf profile below,
with almost no time left in GetById misses, is the strong evidence. DeltaBlue does not: +1.1% with overlapping ranges.
Earley-Boyer is slightly down; a second run of 10 alternating pairs
gave 14108 vs 13958 (0.989), ranges overlapping, so a ~1% loss at the
edge of noise. TypeScript is flat.

Profiles (`perf record -F 2000`, one run each):

- Richards on a perf-prof build at head (`cmake-build-perf`,
  `-Xperf-prof`, `perf inject --jit`): `_sh_ljs_get_by_id_rjs` and
  `_jit_get_by_id` have no samples and `getNamedDescriptorUnsafe` has
  0.02%. The ~9% in getById misses that the Motivation section
  measured is gone: 97.3% of samples are in JIT code and 1.9% in the
  `hermes` binary, whose largest symbol is `_sh_ljs_to_boolean` at
  1.45%.
- DeltaBlue's runtime share is unchanged (base 42.9%, head 42.8%), and
  so is `getNamedDescriptorUnsafe` (14.1% in both). DeltaBlue builds
  three-level hierarchies (`inheritsFrom` places an `Inheriter`
  object between each prototype and its parent's), so method reads on
  constraint objects resolve two or more prototypes up. The read cache
  does not cache those, so nothing is recorded and such sites demote
  (`NumByIdDemotions` 43). This is the "deeper than the immediate
  prototype" non-goal, and the likely reason DeltaBlue did not move;
  the misses were not attributed site by site.
- Earley-Boyer's runtime share is unchanged (63.1% vs 63.2%) and no
  new runtime symbol appears; `getNamedDescriptorUnsafe` drops from
  9.3% to 8.3%.

Recompiles at head vs base (`-Xjit-emit-counters`): Richards 2 vs 0,
DeltaBlue 12 vs 2, RayTrace 9 vs 2, Earley-Boyer 3 vs 3, TypeScript
334 vs 80. `NumByIdDemotions` at head: 0, 43, 55, 4 and 2088.

GetById declines now drive recompiles, and the extra compile work is a
real cost of this tier. TypeScript recompiles about 4x as often (334
vs 80) for a flat score, and demotes 2088 GetById sites on the way.
DeltaBlue (12 vs 2, 43 demotions) and RayTrace (9 vs 2, 55 demotions)
also recompile far more often. On RayTrace the gain pays for it; on
DeltaBlue, it buys nothing measurable.

# JIT: GetById on string receivers

Tracks dz `01a16b01-7ed6` ("JIT: GetById on string receivers always takes
the full C++ lookup").

## Problem

Under `-Xjit`, Octane TypeScript spends about 8% of its cycles in GetById
on string primitive receivers: 22.2M slow-path calls from 49 sites, led
by `charCodeAt` (44%) and `length` (27%) (dz `01a16b01`, measured at
`e5c158faa`). About 13M are in the harness's checksum loop
(`benchmarks/octane/typescript.js:472-477`, `s.length` and
`s.charCodeAt(i)`), 7.1M in the scanner's `peekChar`.

Every one of them takes the generic path:

- The emitted fast path sends any non-object straight to the slow call
  (`lib/VM/JIT/x86-64/JitEmitter-property.cpp:940-941`,
  `lib/VM/JIT/arm64/JitEmitter-property.cpp:1048-1049`).
- Recording ignores non-objects (`lib/VM/JIT/JitHandlers.cpp:765`), so a
  string-only site records nothing and is demoted to a direct
  `_sh_ljs_get_by_id_rjs` call after three unchanged threshold crossings
  (`lib/VM/JIT/JitCompiler.cpp:453-469`).
- That helper reaches `String.prototype` through
  `getPrimitivePrototype` (`lib/VM/Operations.cpp:721-740`) and a hash
  probe, four out-of-line calls, and never reads or fills the read
  property cache for a primitive receiver.

## Goal and success criteria

Serve GetById on string receivers inline in JIT code, for `length` and
for data properties of `String.prototype` itself.

- TypeScript: string-receiver GetById slow calls drop by more than 90%,
  measured with uncommitted instrumentation like dz `01a13df8-970b`'s,
  and the JIT score rises beyond noise.
- No Octane benchmark regresses beyond noise under the JIT.
- The interpreter is untouched: no interpreter or StaticH source
  changes.
- Both backends; HV64, HV32 and boxed.

## Non-goals

- Calling the loaded method faster: a direct call to a known native,
  or `charCodeAt` inline (dz `01a16b01` approach B, about another 5% of
  TypeScript). A separate design.
- Properties inherited by `String.prototype` (from `Object.prototype`),
  accessors, and receivers of other primitive types.
- `TryGetById*` (globals, never strings), `GetByIdWithReceiver*`, and
  sites with `cacheIdx == PROPERTY_CACHING_DISABLED`.
- The interpreter's and StaticH's string lookups (dz `01a16b01`
  approach 3).

## Background

**The site record.** `JitByIdSiteRecord`
(`include/hermes/VM/JIT/JitFunctionData.h:262-301`) holds up to four
shape entries plus `seen`, `megamorphic`, `changed`,
`unchangedCrossings`, the emitted-tier fields `chainEmitted` and
`chainEntryEmitted`, and the helper slot. A recompile deep-copies every
record into the candidate and resets the emitted-tier fields
(`lib/VM/JIT/JitCompiler.cpp:125-160`).

**Recording.** `_jit_get_by_id` (`lib/VM/JIT/JitHandlers.cpp:962-1017`)
forwards to `_sh_ljs_get_by_id_rjs`, then harvests with
`recordByIdObservation` (`:754-796`) while the body is current and the
helper slot still holds `_jit_get_by_id` (`:1002`), then counts a
decline (`:1013-1015`). The harvest sets `seen` first (`:760-763`), then
returns early for a non-object (`:765`).

**Recompile and demotion.** At a decline-threshold crossing,
`considerRecompile` runs the demotion pass, then recompiles only if some
progress term holds (`lib/VM/JIT/JitCompiler.cpp:420-511`). For GetById
the progress term is `byIdSiteCanProgress` (`:316-322`): a live entry the
current body did not emit. The same predicate keeps a site from being
demoted (`:456-458`). A change to the record therefore resets the
demotion count but does not by itself cause a recompile.

**Strings.**

- Every `StringPrimitive` kind keeps `lengthAndFlags_` in the base class
  (`include/hermes/VM/StringPrimitive.h:40`); the length is
  `lengthAndFlags_ & LENGTH_MASK` (`:62`), below 2^30.
  `RuntimeOffsets::stringPrimitiveLengthAndFlags` and
  `stringPrimitiveLengthMask` exist (`lib/VM/JIT/RuntimeOffsets.h:64-67`),
  as do `emit_stringprim_get_length_and_flags` (x86
  `JitEmitter-internal.h:954-961`; arm64 `JitEmitter-internal.h:873`,
  `JitEmitter-internal.cpp:276`).
- A primitive string's prototype is always `runtime.stringPrototype`
  (`lib/VM/Operations.cpp:740`), a `PinnedValue<JSString>`
  (`include/hermes/VM/RuntimeHermesValueFields.def:25`, expanded at
  `include/hermes/VM/Runtime.h:929`). `PinnedValue` privately inherits
  `PinnedHermesValue` (`include/hermes/VM/Handle.h:200`), so the field is
  a HermesValue in memory. `RuntimeOffsets` is a friend of `Runtime`
  (`Runtime.h:1344`). `String.prototype` is a non-writable,
  non-configurable property of `String`, so the object never changes;
  its class does.
- A GetById name is never an array index: the compiler turns such names
  into numbers (`lib/BCGen/Lowering.cpp:240-256`). The only own property
  of a primitive string a GetById can name is `length`
  (`lib/VM/Interpreter-slowpaths.cpp:1576` special-cases it).

**Tags.** In registers a value is always the 64-bit HermesValue,
whatever the heap mode. `emit_sh_ljs_get_tag` is one arithmetic shift
(x86 `JitEmitter-internal.h:70-77`, arm64 `:28-37`); the object and
string tag compares only set flags and leave the tag register intact
(x86 `:102-120`, arm64 `:58-78`). `emit_sh_ljs_is_object` and
`emit_sh_ljs_is_string` are exactly get-tag plus one compare (x86
`:125-142`, arm64 `:152-175`). None of these has a heap-mode variant.

**Class guards.** A lazy JIT ID guard on a class is sound for a slot
load because of this invariant: while an eligible class (not a no-cache
dictionary) stays an object's current class, each of its own properties
keeps its slot and its data or accessor kind. The existing own entries
rely on the same argument. Starting from an eligible class:

- deleting a property from a non-dictionary yields a cacheable
  dictionary, and deleting from a cacheable dictionary yields a fresh
  no-cache dictionary (`lib/VM/HiddenClass.cpp:425-434`);
- changing a property's flags yields a transition, or for a cacheable
  dictionary a fresh no-cache dictionary (`:595-611`; the non-dictionary
  flags transition from `:613`).

Exceptions, none of which affects a load from an existing slot:

- a dictionary adds properties in place, keeping its class
  (`:455-482`); existing slots do not move (see Progress for what this
  means for a name that was absent);
- `updatePropertyFlagsWithoutTransitions` changes flags in place
  (`:1026-1058`); its only callers (`lib/VM/Runtime.cpp:2012`, `:2025`)
  clear writable and configurable and set staticBuiltin, never the
  data/accessor kind;
- a dictionary's parent changes in place (`:830-835`), which cannot
  override an own property.

A no-cache dictionary can be changed in place by deletions and flag
changes (`:432-434`, `:598-611`); such classes are never eligible
(`lib/VM/JIT/JitHandlers.cpp:542-546`).

## Design

### Recording

`JitByIdSiteRecord` gains:

```cpp
/// At least one string receiver completed a recording call here.
/// Observed-tier: carried forward, never cleared.
uint8_t sawString = 0;
/// What the owning body prepared for string receivers at this site.
/// Emitted-tier: reset to NotPrepared on carry-forward.
JitByIdStringTier stringTier = JitByIdStringTier::NotPrepared;
/// Method, Absent and Ineligible only: lazy JIT ID of String.prototype's
/// class when the owning body prepared the site; 0 if none could be
/// assigned. Emitted-tier: reset to 0 on carry-forward.
uint16_t stringProtoId = 0;
/// Absent only: that class's property count when the owning body
/// prepared the site. Emitted-tier: reset to 0 on carry-forward.
uint32_t stringProtoCount = 0;
```

with

```cpp
/// What a body prepared for string receivers at a GetById site.
enum class JitByIdStringTier : uint8_t {
  /// The body did not prepare the site: it was compiled before the site
  /// saw a string.
  NotPrepared,
  /// Prepared, nothing emitted: the name is not an own property of
  /// String.prototype's class, which is eligible and has a lazy ID.
  Absent,
  /// Prepared, nothing emitted: the name is an accessor, the class is not
  /// eligible, or no lazy ID could be assigned.
  Ineligible,
  /// The length block.
  Length,
  /// A method block guarded by stringProtoId.
  Method,
};
```

The kind is explicit; nothing is encoded by nullness of `stringProtoId`
(DESIGN-PRINCIPLES.md, 2).

**Harvest.** In `recordByIdObservation`, before the non-object return at
`JitHandlers.cpp:765`: if the receiver is a string and `!sawString`, set
`sawString` and `changed`, and return true (harvested, which also skips
the chain walk at `:1006-1009`). A later string receiver changes
nothing: the flag marks the transition, not repeats (DESIGN-PRINCIPLES.md,
4). Everything else about the harvest is unchanged.

Change from the approved conversation: the conversation had the harvest
also store `String.prototype`'s current class ID and set `changed` when
it differed. That cannot cause a recompile (Background: a change only
resets the demotion count), so the detection moves into the progress
term below, and the harvest stores nothing but the bit.

### Progress

`byIdSiteCanProgress` (`JitCompiler.cpp:316-322`) also returns true when

```
s.sawString &&
  (s.stringTier == NotPrepared ||
   (s.stringTier != Length &&
    currentStringProtoId(runtime) != s.stringProtoId) ||
   (s.stringTier == Absent &&
    current class is a cacheable dictionary &&
    current class's property count != s.stringProtoCount))
```

where `currentStringProtoId` reads `String.prototype`'s class's
`lazyJITId_` field without assigning one
(`include/hermes/VM/HiddenClass.h:391`); an unassigned class reads 0.
Nothing in the predicate allocates. The predicate and its caller
`byIdShapeProgress` gain a `Runtime &` parameter; both callers have one
(`JitCompiler.cpp:420-511`).

The count term covers the one way a class can gain a property without
changing identity: a dictionary adding it in place (Background, Class
guards). While the same eligible dictionary stays current, its count
only grows, since deleting from it yields a new class, so a different
count means an addition. It is checked only for Absent: an accessor
(Ineligible) stays an accessor while the class is unchanged, and a
Method slot stays valid. An unrelated addition also differs the count
and costs one recompile, bounded by the budget.

Traced per input class:

| case | after the next crossing |
|---|---|
| first string at a site the body did not prepare | NotPrepared: progress, recompile prepares it |
| name is a data property of `String.prototype`, block emitted | hits never call the helper; no crossings |
| name absent or inherited; ID and count unchanged | Absent: no progress; demoted after three unchanged crossings, as today |
| an unrelated property added to a cacheable-dictionary `String.prototype` | Absent, count differs: progress; the recompile prepares Absent again with the new count |
| name is an accessor, or the class is a no-cache dictionary; ID unchanged | Ineligible: no progress; demoted |
| `String.prototype`'s class changed after compile (a polyfill adds a method) | Method, Absent or Ineligible, IDs differ: progress, recompile against the new class |
| the name is added to a cacheable-dictionary `String.prototype` after compile | Absent, same ID, count differs: progress, recompile emits Method |
| the new class has no lazy ID yet, the stored ID non-zero | reads 0, differs: progress; the recompile attempts to assign one |
| Ineligible with ID 0, then a class change to another class without an ID | reads 0 = 0: no progress; values stay correct through the slow call |
| lazy IDs exhausted (`-Xjit-hc-id-limit=0`) | Ineligible with ID 0, reads 0: no progress; demoted |
| class keeps changing | at most one recompile per crossing that finds a progress condition (several changes between crossings coalesce), bounded by `recompileBudget` |
| `length` | Length: no class dependency, never progresses again |
| non-string primitive | not recorded, as today |

Every "progress, recompile" row needs a decline-threshold crossing from
some path that still counts declines in the current body (a recording
GetById or ByVal helper, or `_jit_put_by_id`,
`lib/VM/JIT/JitHandlers.cpp:353-361`), with the JIT enabled and budget
left: `considerRecompile` runs the progress check even when no recording
slot remains (`JitCompiler.cpp:427-428`, `:473-513`). A demoted site's
string state still counts as progress, as its entries already do,
because `byIdShapeProgress` scans every record (`:326-333`). So:

- a demoted site's string misses never cause a crossing themselves: the
  plain helper and `_jit_get_by_id_refresh` (`JitHandlers.cpp:1020-1056`)
  neither harvest strings nor count declines;
- strings first seen after demotion never set `sawString`;
- an emitted block invalidated after demotion is repaired only if some
  other path in the body crosses the threshold; otherwise values stay
  correct through the slow call.

### Preparation

A new shared function in `lib/VM/JIT/JitHandlers.{h,cpp}`, next to
`prepareByIdPolyChain` (`JitHandlers.cpp:620`):

```cpp
/// Prepare GetById site \p site, whose name is \p name, for string
/// receivers, and record the result in site.stringTier and
/// site.stringProtoId. \p lazyId is as for prepareByIdPolyChain.
/// \return the tier; for Method, also \p slot.
JitByIdStringTier prepareByIdString(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    SymbolID name,
    llvh::function_ref<uint16_t(HiddenClass *)> lazyId,
    SlotIndex &slot);
```

1. If `name == Predefined::getSymbolID(Predefined::length)`: Length.
2. `id = lazyId(String.prototype's class)`, the class read from
   `runtime.stringPrototype` immediately before the call. This is the
   only GC safepoint; no raw pointer is held across it.
3. Under `NoAllocScope`, re-read the class and look `name` up with the
   existing non-allocating `chainClassLookup` (`JitHandlers.cpp:685`:
   `tryFindPropertyFast`, falling back to `findPropertyNoAlloc`).
4. With `stringProtoId = id` in every case:
   - Ineligible if `id == 0` or the class fails
     `isEligibleByIdShape(class, nullptr)` (`JitHandlers.cpp:542-546`);
   - Absent, with `stringProtoCount` = the class's property count, if
     the name is not found;
   - Ineligible if it is found and is an accessor;
   - otherwise Method, with the slot.

The emitter calls it only for GetById-family sites (`recording_`) with a
cache entry and `site_->sawString`, before emitting any code for the
instruction.

### Emission

Only the layout of sites the preparation gave Length or Method changes.
Every other site emits exactly today's code.

```
    get_tag  temp1, src            ; once, shared by both compares
    cmp      temp1, STRING_TAG
    jne      objCheck
    <string block>                 ; hit: jmp contLab; miss: jmp slowPathLab
objCheck:
    cmp      temp1, OBJECT_TAG     ; tag still in temp1
    jne      slowPathLab
    <today's object path, from get_pointer on>
```

Strings fall through with no taken branch until the block's final jump;
objects at these sites pay one compare and a taken branch, the tag being
extracted once. The block ends in unconditional jumps, so nothing new
sits between the object fast path and `slowPathLab`, which tier 2 falls
into on a miss (`x86-64/JitEmitter-property.cpp:1218`, arm64 `:1313`).

**Register contract.** Registers are planned before any code
(`x86-64/JitEmitter-property.cpp:894-936`, arm64 `:1002-1044`). `temp1`
is allocated while the source is held, so it never aliases it; `temp2` to
`temp4` are allocated after the source FR is freed and may alias its
register; `hwRes` is allocated last and may alias any temp or the source.
Therefore, in the string block:

- the source register is read only by the first instruction after the
  tag compare (the Length block's pointer extraction into `temp1`); the
  Method block never reads it;
- `hwRes` is written only on a hit, as the last write before
  `jmp contLab`;
- a miss jumps to `slowPathLab` having written only `temp1` to `temp4`,
  which the slow call does not read (it reads the source from the frame,
  synced at `:793-794`);
- at `objCheck` the state is today's at the object compare: the tag in
  `temp1`, the source register intact, the other temps free.

A Length site additionally allocates one vector temporary in the
planning phase (`allocTempVecD`, as `JitEmitter-property.cpp:264-265`
does) and frees it with the GP temps.

**Length block.**

```
    get_pointer temp1, src
    load32      temp1, [temp1 + stringPrimitiveLengthAndFlags]
    and         temp1, stringPrimitiveLengthMask
    cvt         vtmp, temp1           ; x86 vcvtsi2sd, arm64 ucvtf: exact below 2^30
    mov         hwRes, vtmp           ; x86 vmovq, arm64 fmov
    jmp         contLab
```

**Method block.** It does not use the receiver.

```
    load64   temp2, [runtime + stringPrototype]
    get_pointer temp2, temp2
    load_cp  temp3, [temp2 + clazz]; decode
    load16   temp3, [temp3 + hiddenClassLazyJITId]
    cmp      temp3, stringProtoId
    jne      slowPathLab
    <emitLoadFromSlot(hwRes, temp2, slot)>   ; arm64: temp4 as its temporary
    <decode, own Emit_sh_shv_decode instance, doneLab = contLab>
    jmp      contLab
```

The block constructs its own `Emit_sh_shv_decode` and calls `emitAll`
once on it, so it never shares the object path's emit-once tail
(`x86-64/JitEmitter-internal.h:540-604`). In boxed mode the decoder
clobbers `xScratch`, which holds nothing here.

`RuntimeOffsets::stringPrototype = offsetof(Runtime, stringPrototype)`,
next to `globalObject` (`RuntimeOffsets.h:32`), with a `static_assert`
that the field is 8 bytes. arm64 loads it with
`emit_load_from_base_offset<8>(dest, xRuntime, xScratch, ofs)`
(`arm64/JitEmitter-internal.h:351-395`), which handles any offset; x86
uses a disp32.

**Counters and dumps.** Both blocks count a hit in a new emitted counter
`NumByIdStringHits` (`include/hermes/VM/JIT/JitCounters.h`), placed after
the guards as `NumByIdChainHits` is (`x86-64/JitEmitter-property.cpp:1146`),
only under `-Xjit-emit-counters`. The blocks are commented
`// Get string length (site N)` and `// Get string method (site N,
slot M)`, N being the site's bytecode offset. The dump that
prints `JIT ById sites: ...` (`JitCompiler.cpp:735-753`) prints a separate
line `JIT ById string sites: L length, M method` when either is
non-zero. Only one existing test sees it:
`test/jit/x86-64/exceptions.js` checks the lines after `JIT ById sites`
with `CHECK-NEXT` under `--match-full-lines` (`:385-386`, `:393-394`)
and gains the new line.

## Testing

Each lit test lives under `test/jit/`, runs in every JIT build (x86-64
and arm64, all heap modes, and the handle-sanitizer tree, with no
`UNSUPPORTED: handle_san`), wraps its work in `main()` with functions
passed as parameters (DESIGN-PRINCIPLES.md, 6), and states in a comment
what it fails on. Flags as `test/jit/getbyid-chain.js:8-10`
(`-Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16`), plus
`-Xjit-max-recompiles` raised wherever a test counts recompiles, so the
budget never masks a progress bug. Expected counts are derived in each
test's comment.

**Attribution.** Each site under test is the only GetById of its name in
its own function, and the block comments carry the site's bytecode
offset: `// Get string length (site N)`, `// Get string method (site N,
slot M)`. `NumByIdStringHits` is global, so each test isolates the
traffic it attributes to it. Eligible cases need positive evidence that
the block was emitted (DUMP) and executed (exact hits). Cases that emit
no block are pinned by exact `NumRecompiles` and `NumByIdDemotions`
with spare budget, with enough traffic that a broken predicate would
recompile again.

| test | exercises | evidence |
|---|---|---|
| `getbyid-string-length.js` | `s.length` over the string kinds below | DUMP Length block; exact hits; exact values |
| `getbyid-string-method.js` | `charCodeAt` (then called), `indexOf`, `substring`; and data properties added to `String.prototype` holding a small integer, `1.1` (boxed in the boxed build: not representable inline, `include/hermes/VM/SmallHermesValue.h:318-336`), a symbol, a string, an object, `undefined`, `null`, `true` | DUMP Method block per site; exact hits; values, so every decoder branch (`x86-64/JitEmitter-internal.cpp:18-148`) runs |
| `getbyid-string-mixed.js` | one `.length` site over arrays, objects and strings; one site over objects with an own `charCodeAt` and over strings | DUMP: the string block and the object path's poly chain at the same site; exact hits counting only the string traffic |
| `getbyid-string-replace.js` | replace `String.prototype.charCodeAt`'s value after a hit | the new function is called; hits continue (same class, slot reloaded) |
| `getbyid-string-redefine.js` | independent scenarios, each starting from a freshly emitted, positively hit Method block on a data property `foo`: (a) redefine `foo` as an accessor with a strict getter; (b) delete `foo`, read while absent, give its freed slot to another property, then re-add `foo`; (c) and (d) the same from a cacheable-dictionary `String.prototype` | the first read after each mutation, before any recompile: (a) the getter runs with a primitive string `this`; (b) `undefined`, then the new value |
| `getbyid-string-class-change.js` | add an unrelated property to `String.prototype` after a hit | exactly one recompile, then hits again |
| `getbyid-string-dict-add.js` | cacheable-dictionary `String.prototype` (made by deleting one added property); `s.foo` prepared Absent; `foo` added before the site demotes | exactly one recompile driven by the count term, then Method hits |
| `getbyid-string-absent.js` | `s.foo` with `foo` absent; `s.hasOwnProperty` (inherited) | `undefined` and the inherited function; exactly one recompile (the NotPrepared one), then demotion |
| `getbyid-string-dict.js` | `String.prototype` made a cacheable dictionary before the compile | Method block emitted and hit |
| `getbyid-string-hcid.js` | `-Xjit-hc-id-limit=0` | values; exactly one recompile, then demotion, with traffic past several more crossings |
| `getbyid-string-demoted.js` | (a) a site demoted to the plain helper on objects, then strings; (b) a site with an emitted chain entry, demoted to `_jit_get_by_id_refresh`, then strings; (c) a mixed site with an emitted Method block, demoted on its object traffic, then a class change while another site in the body still records; (d) as (c) with no other crossing | (a), (b): values, hits 0; (c): one recompile, hits resume; (d): values, no recompile, hits stop. (a) passes before the feature by design: it pins that demotion stays terminal for strings; (b) gets a second recording site so a refresh helper that wrongly recorded strings would recompile |
| `getbyid-string-gc.js` | allocation and `gc()` between positively verified Method and Length hits, in a loop | values and continued exact hits after each collection: the block reloads `runtime.stringPrototype`, which root marking updates (`lib/VM/Runtime.cpp:630-643`) |
| `getbyid-string-other-prim.js` | numbers and booleans at a site that also sees strings | values via the slow call; string hits unchanged |

`getbyid-string-length.js` builds each string kind with a recipe, and a
mutation verifies each recipe produces what it claims:

- ASCII and UTF-16 dynamic strings: concatenation, and
  `String.fromCharCode` with several arguments (one argument returns a
  cached single-character string, `lib/VM/JSLib/String.cpp:428-438`);
- DynamicUniqued: an `Object.keys` name of a non-index property
  (`lib/VM/JSObject.cpp:595-609`), which carries `LENGTH_FLAG_UNIQUED`
  (bit 31);
- a cached hash (`LENGTH_FLAG_HASH`, bit 30): a fresh, flat,
  non-uniqued string (`String.fromCharCode` with several arguments) used
  as a computed property key, which hashes it in place
  (`lib/VM/IdentifierTable.cpp:101-116`; a non-flat string's hash would
  land on its flattened copy, `:111-114`), kept for the length read;
- Buffered: repeated `+=`, and an older prefix kept from before further
  appends, whose logical length differs from its buffer's
  (`include/hermes/VM/StringPrimitive.h:749-760`);
- External: `"\u0100".repeat(65537)`, built at its final length and
  external above the threshold (`String.cpp:1451-1465`,
  `StringPrimitive.h:955-972`).

Mutations, each checked to fail the named test:

- drop the `jne` after the class-ID compare: `getbyid-string-redefine.js`;
- let bit 31 through the length mask, then separately bit 30:
  `getbyid-string-length.js` (the uniqued and the hashed input);
- drop the NotPrepared progress term: length and method tests (hits 0);
- drop the class-ID progress term: `getbyid-string-class-change.js`;
- drop the count term: `getbyid-string-dict-add.js`;
- make the progress predicate true for Ineligible regardless of IDs:
  `getbyid-string-hcid.js` (a second recompile).

**GC during preparation.** The lit tests cannot make the first lazy-ID
assignment for `String.prototype`'s class allocate: the ID table may
have spare capacity (`x86-64/JitEmitter.cpp:719-742`). A unit test next
to `unittests/VMRuntime/JitChainWalkTest.cpp` calls `prepareByIdString`
with a `lazyId` callback that collects before returning an ID, and
checks the Method result and slot for `charCodeAt`. The
handle-sanitizer tree (`cmake-build-x86jit-hsan`) is an ASan build with
MallocGC, whose sanitizing collection moves every live object and frees
the old copy (`lib/VM/gcs/MallocGC.cpp:65-102`), so a class pointer held
across the callback is a use after free that ASan reports. The lit tests run in
that build with `-gc-sanitize-handles=1` on one RUN line
(`include/hermes/VM/RuntimeFlags.h:44-58`; the default rate is 0.01).

Suites: `check-hermes` (lit and unit tests) in `cmake-build-x86jit`
(ASan) and in `cmake-build-x86jit-hsan`, so the preparation unit test
runs under the sanitizing collector; the JIT suites in the HV32 and
boxed x86 trees; the JIT suite and `aarch64/qemu-sanity.sh` in the three
arm64 trees.

## Benchmarks

Octane from precompiled `.hbc`, `taskset -c 2`, five interleaved reps,
median and min–max, JIT release tree (`cmake-build-jit2`) at the base
and the head. Expected: TypeScript faster beyond noise; no benchmark
slower beyond noise. The interpreter needs no run: no interpreter source
changes. String-receiver GetById slow calls in TypeScript, base vs head,
counted by uncommitted instrumentation; the benchmark is already fixed
work, 35 deterministic iterations (`benchmarks/octane/typescript.js:296-330`,
`:418-422`). The instrumentation counts
`_jit_get_by_id` and `_sh_ljs_get_by_id_rjs` calls whose receiver is a
string.

## Delivered (2026-10-27)

Base `58f7ba649` on branch `x86-jit`.

### Differences from the spec

- **Where `prepareByIdString` lives.** It is defined after `jitWalkChain`
  in `JitHandlers.cpp`, not next to `prepareByIdPolyChain`, because it
  uses the `static` `chainClassLookup`, which is defined after the
  latter. Its declaration in `JitHandlers.h` is next to
  `prepareByIdPolyChain`'s.
- **The string progress terms** live in a `JitHandlers.cpp` function,
  `byIdStringCanProgress(Runtime &, const JitByIdSiteRecord &)`, which
  `byIdSiteCanProgress` falls through to. `byIdSiteCanProgress` and
  `byIdShapeProgress` now take `Runtime &` so that it can read
  `String.prototype`'s class.
- **`RuntimeOffsets::stringPrototype`** also asserts 8-byte alignment,
  which arm64's `emit_load_from_base_offset<8>` requires. The assert
  holds in all three heap modes on both backends.
- **Only GetById sites prepare strings**, not the whole GetById family
  (:297): the preparation sits under `recording_`, which is false for
  `TryGetById`.
- **Interim states.** The recording step prepared and
  recorded tiers on both backends but emitted no block (a Length site
  never progresses again, a Method site only on a class change, so
  nothing looped). The x86-64 blocks came next, with the new tests
  gated to x86-64; the arm64 blocks lifted the gate. The arm64 JIT
  suite had no failure after any step that ran it.
- **Sites without a string block** emit the same instructions and
  control flow as before the feature, with one exception: the
  displacement of the recording call's helper slot grew with the record
  (see Sizes). On x86-64 `call qword ptr [r9+192]` became
  `[r9+200]` on HV64, and 160 became 168 on HV32; on arm64 the
  immediate of `ldr x16, [x5, #ofs]` changes the same way. The
  byte-identity checks compared code with and without the string
  blocks on binaries that already had the larger record: on x86-64,
  the recording step's binary against the x86-64 emission step's; on
  arm64, a binary built at the x86-64 emission step against the arm64
  step's. The pre-feature binary was not the baseline, so those checks
  could not see the displacement change.
- **Two existing tests see the dump line, not one** (:390).
  `test/jit/x86-64/exceptions.js` gained the
  `JIT ById string sites: 1 length, 0 method` line under its
  `CHECK-NEXT`s, as planned. `test/jit/x86-64/strings.js` also changed:
  its `churn` reads `acc.length` 2000 times, version 1 is compiled before
  the site has seen a string, and the NotPrepared term now recompiles it
  once into a version 2 with a Length block (no version 3). It gained
  CHECK lines for that compile and the dump line.
- **`-gc-sanitize-handles=1` on one RUN line** means one extra RUN line
  per new lit test, checking the values (OUT) only, so collection timing
  cannot perturb the counter checks. Two exceptions:
  `getbyid-string-gc.js`'s sanitize line also checks the hit counter
  (by design; it is the GC test), and `getbyid-string-bigid.js` has no
  sanitize line: its 4200 `new Function` compiles took about 20 minutes
  under MallocGC with sanitizing, and `-method.js` and `-gc.js` already
  run the Method block under the sanitizer.
- **Scenarios that need a pristine `String.prototype`** (redefine,
  demoted) run one per process, selected by a script argument through
  `hermescli.getScriptArgs()`.
- **`getbyid-string-demoted.js` (b)** adds a second recording site, so
  a refresh helper that wrongly recorded strings would recompile (a
  mutation pins it). Scenario (a) has no mutation in this feature's
  code: it passes before the feature by design.
- **The length mask mutations.** The uniqued receivers (the
  `Object.keys` name, `''` and `'z'`) carry bit 31 only, with no cached
  hash (bit 30). Letting bit 30 through breaks only the hashed
  receiver's length; letting bit 31 through breaks the uniqued ones.
  On arm64 the planned mutation (mask `0x7fffffff`) lets only bit 30
  through, so a second one, the W-form mask `0xBFFFFFFF` (the X form
  cannot encode it), pins bit 31.
- **Tests beyond the table:** `lenSelf` in `-length.js` (a Length site
  whose result register is its source); the `toString` (direct slot)
  and `indexOf` (out of line) slot checks in `-method.js` and the unit
  test; `null` and `undefined` receivers throwing `TypeError` in
  `-method.js`; a Length site `len` in `-method.js` that runs after the
  Method sites have given `String.prototype`'s class a lazy ID, so a
  Length site that progressed would recompile again;
  `getbyid-string-bigid.js` (a lazy ID above 4095, compared on arm64
  through `w16`); `getbyid-string-ineligible.js` (an accessor on
  `String.prototype` gives a site Ineligible against a class that has
  a lazy ID; the site never progresses and demotes after three
  unchanged crossings); BigInt, symbol, `String`-object and throwing
  receivers in `-other-prim.js`; BufferedUTF16 and
  DynamicUniquedUTF16 receivers in `-length.js`, so the Length block
  runs over all eight string kinds; and a BigInt-valued property read
  through a Method block in `-method.js`, which runs the HV32 decoder's
  pointer branch with the BigInt tag.
- **Test anchoring.** `-length.js`'s bytecode check is
  `BC: GetById{{[A-Za-z]*}} [[R:r[0-9]+]], [[R]],` (FileCheck did not
  match a `{{(Short)?}}` group combined with the back-reference).
  `-bigid.js`'s dump checks require the ID load, a four-hex-digit
  compare and the branch on consecutive lines; a bare
  `movzx ... word ptr` matched the read-cache path. `-length.js`'s
  `BC-LABEL` is `Function<lenSelf>(`: the bare name also matched main's
  `CreateClosure`.
- **doc/JIT.md follows the code** where the planned text did not: the
  blocks end in "a jump to `contLab`" (arm64 branches with `b`), the
  conversion is named per backend (`vcvtsi2sd` and `vmovq` on x86-64,
  `ucvtf` on a `w` register and `fmov` on arm64), and the large-ID test
  runs on both backends, so the doc names only its compare through `w16`
  as arm64's.
- **This section** goes in the spec, as for the chain spec; the plan has
  no Delivered section.

### Matrix

The suites below ran on the code and tests of the commit that contains
this section: after the run, only this document changed.

| tree | configuration | result |
|---|---|---|
| `cmake-build-x86jit` | HV64 ASan+Debug, JIT=2, full `check-hermes` | 4495 pass, 6 expected failures, 162 unsupported, 0 fail; JIT suite 181 pass, 8 unsupported |
| `cmake-build-x86jit-hsan` | ASan + MallocGC + handle sanitizer, full `check-hermes` | 4326 pass, 6 expected failures, 232 unsupported, 1 fail: `jit/x86-64/counters-slow-call-kinds.js`, the known flake (dz `01a06156`; alone it then failed 2 of 3 runs); JIT suite alone 112 pass, 77 unsupported, 0 fail; `JitStringPrepTest` 7 of 7 |
| `cmake-build-x86jit-hv32` | HV32 ASan+Debug, JIT suite | 181 pass, 8 unsupported, 0 fail |
| `cmake-build-x86jit-boxed` | BOXED ASan+Debug, JIT suite | 181 pass, 8 unsupported, 0 fail |
| `cmake-build-arm64` | HV64 qemu-user, JIT suite; `aarch64/qemu-sanity.sh` | 158 pass, 31 unsupported, 0 fail; 9 of 9 |
| `cmake-build-arm64-hv32` | HV32 qemu-user, JIT suite; `aarch64/qemu-sanity.sh` | 158 pass, 31 unsupported, 0 fail; 9 of 9 |
| `cmake-build-arm64-boxed` | BOXED qemu-user, JIT suite; `aarch64/qemu-sanity.sh` | 158 pass, 31 unsupported, 0 fail; 9 of 9 |

All 15 `getbyid-string-*.js` tests run (none unsupported) and pass on
all seven trees, including the handle-sanitizer tree.

Every new test was shown to fail before the feature and under a
mutation of the code it pins. Each mutation was then restored, the tree
rebuilt and the tests re-run green. Tasks: 1 records and preparation,
2 recording and progress, 3 x86-64 blocks, 4 arm64 blocks, 5
invalidation, demotion and other receivers; "final" is the final
review's fix wave and "review" the external code review's fixes, which
re-ran the earlier mutations against the extended `-length.js` and
`-method.js`.

| task | mutation | failure |
|---|---|---|
| 1 | A: no `length` early return in `prepareByIdString` | `JitStringPrepTest.Length`: Method, not Length |
| 1 | B: no accessor branch | `AccessorIsIneligible`: Method, not Ineligible |
| 1 | C: no `id == 0` test | `NoIdIsIneligible`: Method |
| 1 | D: no `isEligibleByIdShape` test | `NoCacheDictionaryIsIneligible`: Method |
| 1 | E: `stringProtoCount` not set | `Absent`: count 0, not 41 |
| 1 | P2: `slot = desc.slot + 1` | `MethodCharCodeAt` and `CollectingLazyId`: slot 7, not 6 |
| 1 | F: the class read before the lazy-ID callback and held across it (hsan tree) | `CollectingLazyId`: ASan heap-use-after-free in `isEligibleByIdShape` |
| 2 | A: NotPrepared does not progress | `-absent.js` (not 2) and `-hcid.js` (not 1): `NumRecompiles: 0` |
| 2 | B: Ineligible progresses regardless of IDs | `-hcid.js`: `NumRecompiles: 8`, not 1 |
| 2 | C: every string receiver sets `changed` | `-absent.js` (not 2) and `-hcid.js` (not 1): `NumByIdDemotions: 0` |
| 3 | A: NotPrepared does not progress | `-length.js` and `-method.js`: no version 2 |
| 3 | B: mask lets bit 31 through | `-length.js`: uniq, `''` and `'z'` read 2147483663, 2147483648, 2147483649 |
| 3 | C: mask lets bit 30 through | `-length.js`: hashed reads 1073741830 |
| 3 | D: no slot-value decode | `-method.js` on HV32 and BOXED: the process dies, no output (HV64 needs no decode) |
| 3 | E: the object check after the block tests for a string | `-mixed.js`: `NumByIdDemotions: 2`, not 0 |
| 3 | F: `String.prototype` taken at compile time, not loaded from the runtime (hsan tree) | `-gc.js`: ASan SEGV in emitted code |
| 3 | G: slot value taken at compile time | `-replace.js`: `0`, not `10` |
| 3 | result written before the Length block reads the source | `-length.js`: crash (`lenSelf` alone: SEGV on the zero page) |
| 3 | out-of-line slot loaded as direct | `-method.js`: `TypeError: 5.09278990266e-313 is not a function` |
| 3 | direct slot loaded through `propStorage` | `-method.js`: `ts` reads wrong on its 4 version-2 hits |
| 3 | `null`/`undefined` take the string block | `-method.js`: `no throw no throw`, not `TypeError TypeError` |
| 3 | no Length early return in `byIdStringCanProgress` | `-method.js`: `len` gets a version 3 (`NumRecompiles: 14`, not 13) |
| 4 | A: arm64 mask `0x7fffffff` (bit 30 through) | `-length.js`: hashed reads 1073741830 |
| 4 | A2: arm64 W-form mask `0xBFFFFFFF` (bit 31 through) | `-length.js`: uniq, `''` and `'z'` read 2147483663, 2147483648, 2147483649 |
| 4 | B: no slot-value decode | `-method.js` on HV32 and BOXED: `TypeError: ... is not a function` |
| 4 | C: `emit_cmp_imm32` given `xTemp3` as its temporary | `-bigid.js`: `cmp w2, w2`, `NumByIdStringHits: 90`, not 82; the dump check rejects the code |
| 4 | D: the object check after the block tests for a string | `-mixed.js`: `NumByIdDemotions: 2`, not 0 |
| 5 | A: x86-64 class-ID `jne` removed | `-redefine.js` (all scenarios: a cast assertion on the accessor, an empty value on the deleted slot), `-demoted.js` (64 hits), `-class-change.js`, `-bigid.js` |
| 5 | B: no class-ID progress term | `-class-change.js`: 1 recompile, 4 hits; `-demoted.js` (c) |
| 5 | C: no count term | `-dict-add.js`: 1 recompile, 1 demotion, 0 hits |
| 5 | D: any pointer tag takes the string block | `-other-prim.js`: a String object reads length 0, a BigInt reads 1; also `-demoted.js` (c), (d) and `-mixed.js` |
| 5 | E: dictionaries made Ineligible | `-dict.js`: 0 hits; also `-dict-add.js` and `-redefine.js` (c), (d) |
| 5 | F: the refresh helper records strings | `-demoted.js` (b) only: 2 recompiles, 8 hits |
| 5 | G: arm64 class-ID `b.ne` removed | `-class-change.js`, `-redefine.js`, `-demoted.js` (c), `-bigid.js` |
| final | Ineligible progresses whatever the ID | `-ineligible.js`: `NumRecompiles: 8`, not 1 |
| review | NotPrepared does not progress | `-length.js` and `-method.js`: no version 2, `NumByIdStringHits: 0` |
| review | x86-64 mask lets bit 31 through | `-length.js` with the new receivers: uniq, uniqU, `''` and `'z'` read 2147483663, 2147483658, 2147483648, 2147483649 |
| review | x86-64 mask lets bit 30 through | `-length.js`: hashed reads 1073741830 |
| review | arm64 mask `0x7fffffff` (bit 30 through) | `-length.js`: hashed reads 1073741830 |
| review | arm64 W-form mask `0xBFFFFFFF` (bit 31 through) | `-length.js`: uniq, uniqU, `''` and `'z'` read 2147483663, 2147483658, 2147483648, 2147483649 |
| review | no Length early return in `byIdStringCanProgress` | `-method.js` with `gBig`: `len` gets a version 3 (`NumRecompiles: 15`, not 14) |
| review | no slot-value decode (x86-64 HV32, arm64 HV32) | `-method.js`: no output (it stops at `cc`, the first Method site); `gBig` alone returns the right value on 16 of 20 calls, the 4 block hits returning the undecoded slot |

### Sizes

`JitByIdSiteRecord` and the offset of its helper pointer (read from the
`call qword ptr [r9+N]` of a recording site in the x86-64 dump):

| mode | before | after |
|---|---|---|
| HV64 | 200 B, helper at 192 | 208 B, helper at 200 |
| HV32 | 168 B, helper at 160 | 176 B, helper at 168 |

### Benchmarks

Both trees are `cmake-build-jit2`: Release, clang, `HERMESVM_ALLOW_JIT=2`,
`HEAP_HV_64`, Hades, no debugger. The base tree is a worktree of
`58f7ba649`, configured with the same values; the head is the feature's
last commit. Each tree compiled its own bytecode, and every run pinned
one core:

```
<tree>/bin/hermesc -O -emit-binary -out <build>/<b>.hbc benchmarks/octane/<b>.js
taskset -c 2 <tree>/bin/hermes -Xjit <build>/<b>.hbc
```

Five reps. In each rep every benchmark ran on both builds back to back,
base first in odd reps and head first in even ones. Median and min-max
of the five scores; "faster" or "slower" only when the two ranges do not
overlap. No run failed.

| benchmark | base median [min-max] | head median [min-max] | head/base | verdict |
|---|---|---|---|---|
| richards | 7809 [7336-7996] | 7551 [6842-8225] | 0.967 | noise |
| deltablue | 7465 [7213-7518] | 7432 [7306-7465] | 0.996 | noise |
| crypto | 7163 [7130-7168] | 7207 [7177-7226] | 1.006 | faster |
| raytrace | 12825 [12664-12850] | 12493 [12309-12998] | 0.974 | noise |
| earley-boyer | 13379 [13070-13441] | 13394 [12945-13498] | 1.001 | noise |
| regexp | 931 [927-943] | 937 [926-944] | 1.006 | noise |
| splay | 4767 [4725-4787] | 4751 [4678-4816] | 0.997 | noise |
| navier-stokes | 14471 [14395-14485] | 14456 [14395-14456] | 0.999 | noise |
| pdfjs | 19990 [19970-20149] | 20866 [20764-20907] | 1.044 | faster |
| mandreel | 4798 [4777-4814] | 4773 [4744-4805] | 0.995 | noise |
| gbemu | 20598 [20241-20740] | 20679 [20398-20720] | 1.004 | noise |
| box2d | 23857 [23338-24302] | 23809 [23574-24063] | 0.998 | noise |
| zlib | 6414 [6218-6439] | 6426 [6403-6450] | 1.002 | noise |
| typescript | 51916 [51207-51958] | 55389 [54653-55631] | 1.067 | faster |
| geomean | | | 1.004 | |

TypeScript is 6.7% faster, beyond noise; no benchmark is slower beyond
noise.

**String-receiver slow calls in TypeScript.** Uncommitted
instrumentation in `_sh_ljs_get_by_id_rjs` (`lib/VM/StaticH.cpp`)
counted the calls whose receiver is a string, by name, and printed the
counts at exit. Every JIT GetById slow call reaches that function once:
the recording and refresh helpers forward to it, a demoted site calls
it directly, and the interpreter never does. The same patch went into
both trees; each ran its own `typescript.hbc` with the command above,
twice, and the counts repeated exactly (the benchmark does fixed work:
35 deterministic iterations).

| | total | `length` | `charCodeAt` | other names |
|---|---|---|---|---|
| base | 22,228,231 | 8,033,473 | 13,307,034 | 887,724 |
| head | 11,146 | 277 | 234 | 10,635 |

Head makes 0.05% of base's string-receiver slow calls.

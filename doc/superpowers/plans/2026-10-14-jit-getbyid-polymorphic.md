# JIT Polymorphic GetById Tier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make polymorphic GetById sites (several receiver classes,
own or immediate-prototype data properties) stay inline on the x86-64
JIT, by recording per-site shapes in a recording helper and emitting a
polymorphic chain on recompile.

**Architecture:** GetById sites join the version-local consumer
records next to the ByVal sites: a per-site `JitByIdSiteRecord` with up
to 4 weakly held shapes, filled by a new recording helper
`_jit_get_by_id` that harvests the interpreter read cache after the
normal lookup. A new progress term drives recompilation; the emitter
turns live recorded shapes into an inline chain (own entries, then
prototype entries) ahead of today's generic live-cache tier. Demotion,
carry-forward, retirement and the helper-slot indirection reuse the
ByVal machinery.

**Tech Stack:** C++17, asmjit x86-64, lit/FileCheck.

**Plan review:** Codex, 3 rounds (5 blockers, 9 major issues found
and resolved; final verdict READY TO EXECUTE).

**Spec:** doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md
(Codex-reviewed over 4 rounds, all findings CLOSED; binding). Read it
in full before starting any task. Its templates are
doc/superpowers/specs/2026-09-11-jit-getbyval-design.md and
doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md.

## Global Constraints

- x86-64 only. The arm64 emitter is untouched; arm64 GetById keeps the
  direct `_sh_ljs_get_by_id_rjs` call. Shared files (JitCompiler.cpp,
  JitHandlers.cpp, JitFunctionData.h, CodeBlock.cpp) must keep building
  and behaving identically for arm64, where `byIdSites` stays empty.
- Only `GetById`, `GetByIdShort`, `GetByIdLong` (all reach
  `Emitter::getById`) record. `TryGetById*` and `GetByIdWithReceiver*`
  are unchanged. Sites with `cacheIdx == hbc::PROPERTY_CACHING_DISABLED`
  keep the direct helper call.
- K = `JitByIdSiteRecord::kMaxEntries` = 4.
- Eligibility = the read cache fill's predicates: the property-holding
  class is not `isDictionaryNoCache()`; for a prototype entry the
  receiver class is not `isDictionary()`.
- Recording helper order is fixed by the spec: forward → root result in
  `Locals` → staleness check (`vd == current` and `site.helper ==
  _jit_get_by_id`) → record under `NoAllocScope` → count / possibly
  `considerRecompile` → return the pinned result. A throwing forwarded
  call is neither recorded nor counted.
- `changed` is set by entry append/overwrite, by the first `seen`, and
  by `megamorphic` only on its 0 → 1 transition.
- No bytecode or BytecodeVersion.h changes.
- Every runtime C++ change under lib/VM/ (including lib/VM/JIT/) is
  written after invoking the `gc-safe-coding` skill.
- 80 columns, 2-space indent, doc comment on every declaration, member
  variables `_` suffixed only where the surrounding class already does.
- Never bare `cd`; use paths or `( cd dir; cmd; )`.
- Build trees (configure once, per doc/JITTesting.md "Configure
  commands"): `cmake-build-x86jit` (HV64 ASan+Debug, the default),
  `cmake-build-x86jit-hv32`, `cmake-build-x86jit-boxed`,
  `cmake-build-x86jit-rel`, plus `cmake-build-x86jit-malloc` (same as
  `cmake-build-x86jit` with `-DHERMESVM_GCKIND=MALLOC`) and
  `cmake-build-x86jit-hsan` (same with BOTH `-DHERMESVM_GCKIND=MALLOC`
  and `-DHERMESVM_SANITIZE_HANDLES=ON`: MallocGC's sanitizer moves every
  live object at every allocation; Hades' does not guarantee that).
- JIT suite on a tree:
  `LIT_FILTER='jit/' cmake --build <tree> --target check-hermes`.
  Full suite: same without `LIT_FILTER`. Always rebuild a tree before
  running its suite (check-hermes does build `hermes`, but rebuild
  explicitly after any mutation/restore).
- Prove-can-fail: every new test is shown to FAIL with the code it
  pins disabled; restore, REBUILD, and re-run green.
- Commits: one per task, message per
  tightening-commit-messages, ending with exactly:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MxvtDWnnS16sfyuVjoNLwC
  ```
- The spec and this plan are already committed; update the spec's
  "Delivered" section in Task 5.

## Review Focus

Inputs the spec implies but its test list does not name, with the
expected behavior; each has a test in the owning task:

1. A receiver created by `Object.create(null)` whose class equals a
   recorded prototype entry's receiver class reaches the prototype
   block with a null parent: must fall to the generic tier/helper and
   read `undefined` (Task 3, getbyid-poly-edge.js).
2. A Proxy receiver at a site with an emitted chain: the class never
   matches, the trap result is returned (Task 3, getbyid-poly-edge.js).
3. Properties in indirect (out-of-line) slots, own and on the
   prototype: the chain's `emitLoadFromSlot` indirect path loads the
   right value (Task 3, getbyid-poly-edge.js).
4. A getter that throws at a recording site, inside try/catch, many
   times: the exception propagates every time, nothing crashes, the
   site neither records nor counts those calls (Task 2,
   getbyid-record-throw.js).
5. Primitive receivers (`"str".length`, numbers) at a site with an
   emitted chain: go straight to the helper and return the primitive's
   property (Task 3, getbyid-poly-generic-join.js).

---

## File Structure

- `include/hermes/VM/JIT/JitFunctionData.h` — `JitByIdShapeEntry`,
  `JitByIdSiteRecord`, `byIdSites`, `recordingSites` (renamed from
  `recordingByValSites`), `JitFunctionData::compiling`.
- `lib/VM/CodeBlock.cpp` — weak tracing of ById records of `current`
  and `compiling`.
- `lib/VM/JIT/JitCompiler.cpp` — carry-forward, `compiling` lifetime,
  version derivation, install count, retirement, demotion pass,
  progress term, dump line.
- `lib/VM/JIT/JitHandlers.{h,cpp}` — `_jit_get_by_id`,
  `isEligibleByIdShape`, `addByIdShape`, `recordByIdObservation`.
- `include/hermes/VM/JIT/JitCounters.h` — `NumByIdDemotions`.
- `lib/VM/JIT/x86-64/JitEmitter.h` — `byIdSiteRecord()`,
  `getByIdImpl(..., bool recording)`, `DECL_GET_BY_ID` change.
- `lib/VM/JIT/x86-64/JitEmitter-property.cpp` — `GetByIdImpl`: site
  record, recording helper call, legacy specializations recorded,
  polymorphic chain.
- `test/jit/x86-64/getbyid-*.js` — new tests.
- `doc/JIT.md`, spec "Delivered" — Task 5.

---

### Task 1: Records, GC tracing, carry-forward, `compiling`

Inert plumbing: no site records are created yet, so behavior is
unchanged. The existing JIT suite is the gate.

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h`
- Modify: `lib/VM/CodeBlock.cpp:334-350` (`markWeakElementsInCaches`)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (Compiler ctor ~103-145, new
  dtor, `compileCodeBlockImpl` ~483, install ~547-573,
  `demoteAllSitesOnRetirement` ~327, every `recordingByValSites` use)

**Interfaces:**
- Produces: `struct JitByIdShapeEntry { WeakRoot<HiddenClass> objClass,
  protoClass; uint16_t slot; uint8_t isProto, emitted, unemittable;
  bool isLive() const; }`;
  `struct JitByIdSiteRecord { static constexpr unsigned kMaxEntries = 4;
  uint32_t siteId; llvh::SmallVector<JitByIdShapeEntry, 4> entries;
  uint8_t seen, megamorphic, changed, unchangedCrossings, chainEmitted;
  void *helper; }`;
  `JitConsumerRecords::byIdSites` (`std::deque<JitByIdSiteRecord>`),
  `JitConsumerRecords::recordingSites` (uint32_t),
  `JitByIdSiteRecord &JitConsumerRecords::findOrCreateByIdSite(uint32_t)`,
  `JitByIdSiteRecord *JitConsumerRecords::findByIdSite(uint32_t)`,
  `JitVersionData *JitFunctionData::compiling`.

- [ ] **Step 1: Configure and build the reference tree**

If `cmake-build-x86jit` does not exist, configure it with config 2 of
doc/JITTesting.md:

```bash
cmake -B cmake-build-x86jit -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" -DHERMESVM_ALLOW_JIT=2
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all JIT tests pass (record the pass count; it is the
baseline for every later task).

- [ ] **Step 2: Add the record types**

In `include/hermes/VM/JIT/JitFunctionData.h`, add
`#include "hermes/VM/WeakRoot.h"` to the includes, forward-declare
`class HiddenClass;` next to `class CodeBlock;`, and add after
`JitByValSiteRecord`:

```cpp
/// One observed GetById shape: a receiver class and, for a prototype
/// hit, the immediate prototype's class, plus the slot to load. See
/// the 2026-10-14 polymorphic GetById spec, "Records".
struct JitByIdShapeEntry {
  /// The receiver's hidden class.
  WeakRoot<HiddenClass> objClass{nullptr};
  /// The immediate prototype's hidden class; meaningful only if isProto.
  WeakRoot<HiddenClass> protoClass{nullptr};
  /// Slot in the receiver (own entry) or in the prototype.
  uint16_t slot = 0;
  /// Set when recorded, never changed: a prototype entry whose
  /// protoClass the GC cleared is dead, never an own entry.
  uint8_t isProto = 0;
  /// The owning body emitted an inline check for this entry. Carried
  /// forward: emitted classes are pinned by their lazy JIT IDs and every
  /// later version emits the entry again.
  uint8_t emitted = 0;
  /// A compile could not emit this entry (no lazy JIT ID). Never
  /// progress; reset on carry-forward.
  uint8_t unemittable = 0;

  /// Default: an empty, dead entry.
  JitByIdShapeEntry() = default;
  /// Copy every field. Spelled out because CompressedPointer deletes its
  /// copy assignment (CompressedPointer.h), so WeakRoot members make the
  /// implicit operations ill-formed; copying a weak root needs no
  /// barrier (it is "just moved around", WeakRoot.h).
  JitByIdShapeEntry(const JitByIdShapeEntry &other) {
    *this = other;
  }
  /// See the copy constructor.
  JitByIdShapeEntry &operator=(const JitByIdShapeEntry &other) {
    objClass = other.objClass.getNoBarrierUnsafe();
    protoClass = other.protoClass.getNoBarrierUnsafe();
    slot = other.slot;
    isProto = other.isProto;
    emitted = other.emitted;
    unemittable = other.unemittable;
    return *this;
  }

  /// \return true if every class this entry needs is still alive.
  bool isLive() const {
    return objClass && (!isProto || protoClass);
  }
};

/// One GetById site's observed shapes and what the owning body emitted
/// for it. Observed fields are written by _jit_get_by_id alone.
struct JitByIdSiteRecord {
  /// Maximum number of shapes kept per site.
  static constexpr unsigned kMaxEntries = 4;

  /// Bytecode offset of the GetById instruction.
  uint32_t siteId;
  /// Observed shapes, at most kMaxEntries; dead entries are reused.
  llvh::SmallVector<JitByIdShapeEntry, kMaxEntries> entries;
  /// The recording helper completed at least one call here. The
  /// demotion rule's "observed" test.
  uint8_t seen = 0;
  /// A distinct live shape arrived while entries was full.
  uint8_t megamorphic = 0;
  /// Same meaning as JitByValSiteRecord::changed.
  uint8_t changed = 0;
  /// Same meaning as JitByValSiteRecord::unchangedCrossings.
  uint8_t unchangedCrossings = 0;
  /// The owning body emitted a polymorphic chain here. Emitted-tier
  /// field: reset on carry-forward; feeds the dump line only.
  uint8_t chainEmitted = 0;
  /// Per-site mutable helper slot: _jit_get_by_id while recording,
  /// _sh_ljs_get_by_id_rjs after demotion. Never flipped back.
  void *helper = nullptr;
};
```

In `JitConsumerRecords`, add the deque and accessors, and rename the
counter (update its doc comment to "ByVal and ById sites"):

```cpp
  /// GetById site records. A deque for the same reason as byValSites:
  /// emitted code embeds the address of a record's helper slot.
  std::deque<JitByIdSiteRecord> byIdSites;

  /// Number of ByVal and ById sites whose helper slot still holds a
  /// recording helper. ...(keep the rest of the old comment)
  uint32_t recordingSites = 0;

  /// \return the ById record for \p siteId, creating it if absent.
  JitByIdSiteRecord &findOrCreateByIdSite(uint32_t siteId) {
    for (auto &s : byIdSites)
      if (s.siteId == siteId)
        return s;
    byIdSites.emplace_back();
    byIdSites.back().siteId = siteId;
    return byIdSites.back();
  }

  /// \return the ById record for \p siteId, or null if absent.
  JitByIdSiteRecord *findByIdSite(uint32_t siteId) {
    for (auto &s : byIdSites)
      if (s.siteId == siteId)
        return &s;
    return nullptr;
  }
```

In `JitFunctionData`, add:

```cpp
  /// The candidate record of a compile in progress for this function,
  /// or null. Set by the Compiler constructor, cleared at install and
  /// by the Compiler destructor. CodeBlock::markWeakElementsInCaches
  /// traces its ById records, because a compile can collect
  /// (initHCLazyIDMayAlloc) while the candidate is not yet `current`.
  JitVersionData *compiling = nullptr;
```

Rename every `recordingByValSites` in lib/VM/JIT/JitCompiler.cpp to
`recordingSites` (`grep -rn recordingByValSites lib include` must end
empty).

- [ ] **Step 3: Trace the weak roots**

In `lib/VM/CodeBlock.cpp`, at the end of
`CodeBlock::markWeakElementsInCaches`:

```cpp
#if HERMESVM_JIT
  // Polymorphic GetById shape records hold weak hidden-class references.
  // Only the current body's records and a compile candidate's are read
  // again; retired records are cleared at retirement.
  if (JitFunctionData *jd = jitData_.get()) {
    auto markById = [&acceptor](JitVersionData *vd) {
      if (!vd || !vd->consumerRecords)
        return;
      for (auto &site : vd->consumerRecords->byIdSites) {
        for (auto &e : site.entries) {
          acceptor.acceptWeak(e.objClass);
          acceptor.acceptWeak(e.protoClass);
        }
      }
    };
    markById(jd->current.get());
    markById(jd->compiling);
#ifndef NDEBUG
    // Retired records are not traced, which is sound only because they
    // hold no entries: retirement clears them and the recording helper's
    // staleness gate keeps them clear (spec: "GC"). Check it on every
    // collection, so a violation fails loudly in Debug builds.
    for (auto &vd : jd->retired) {
      if (!vd->consumerRecords)
        continue;
      for (auto &site : vd->consumerRecords->byIdSites)
        assert(site.entries.empty() && "retired ById record repopulated");
    }
#endif
  }
#endif
```

- [ ] **Step 4: Carry-forward, `compiling`, version derivation**

In the Compiler constructor, inside the existing
`if (jd->current && jd->current->consumerRecords)` block, after the
ByVal loop:

```cpp
        for (const auto &s : jd->current->consumerRecords->byIdSites) {
          JitByIdSiteRecord r = s;
          r.changed = 0;
          r.unchangedCrossings = 0;
          r.chainEmitted = 0;
          for (auto &e : r.entries)
            e.unemittable = 0;
          copy->byIdSites.push_back(std::move(r));
        }
```

After that whole `if (JitFunctionData *jd = ...)` block, publish the
candidate (this also creates the function data on a FIRST compile):

```cpp
    // Publish the candidate so a GC during emission traces the weak
    // shape references it carries (spec: "GC").
    codeBlock->ensureJitData(jc.getMaxRecompiles())->compiling =
        candidateVD_.get();
```

Add a destructor to `JITContext::Compiler`:

```cpp
  /// Withdraw the candidate from GC tracing on every exit path,
  /// including the longjmp failure path, which destroys this object
  /// normally.
  ~Compiler() {
    if (JitFunctionData *jd = codeBlock_->getJitData())
      jd->compiling = nullptr;
  }
```

`ensureJitData` now runs before the first install, so in
`compileCodeBlockImpl` change the version derivation to depend on an
installed body, not on the data's existence:

```cpp
  JitFunctionData *priorData = codeBlock_->getJitData();
  unsigned version =
      priorData && priorData->current ? priorData->version + 1u : 1u;
```

At install, clear `compiling` before the candidate is moved:

```cpp
  JitFunctionData *jitData = codeBlock_->ensureJitData(jc_.getMaxRecompiles());
  jitData->compiling = nullptr;
```

and count ById sites in the recording count (the helper identity is
added in Task 2; until then no ById record exists):

```cpp
  if (JitConsumerRecords *cr = candidateVD_->consumerRecords.get()) {
    uint32_t recording = 0;
    for (const auto &s : cr->byValSites)
      recording += isRecordingHelper(s.helper);
    for (const auto &s : cr->byIdSites)
      recording += isRecordingHelper(s.helper);
    cr->recordingSites = recording;
  }
```

In `demoteAllSitesOnRetirement`, before `cr->recordingSites = 0;`:

```cpp
  // A retired body's ById observations are never read again; dropping
  // them means its weak references need no tracing.
  for (auto &s : cr->byIdSites)
    s.entries.clear();
```

- [ ] **Step 5: Build and run the JIT suite**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: same pass count as Step 1. In particular every test that
pins `JIT successfully compiled ... '<f>'` without ` (version N)` for a
first compile must still pass (this is what the version-derivation fix
protects; prove it: revert just that line, rebuild, see
`test/jit/recompile-*.js` failures, restore, rebuild, green).

- [ ] **Step 6: Commit**

```bash
git add include/hermes/VM/JIT/JitFunctionData.h lib/VM/CodeBlock.cpp \
  lib/VM/JIT/JitCompiler.cpp
git commit   # "JIT: add GetById shape records and trace them weakly"
```

---

### Test isolation rule (applies to every new test below)

Process-wide counters (`NumRecompiles`, `NumRecompileChecks`,
`NumByIdDemotions`) see every function. A global variable read inside
a function compiles to `GetByIdShort` on the global object, which is
itself a recording GetById site, and global writes go through
`_jit_put_by_id`; either can add crossings and recompiles that have
nothing to do with the site under test (see the comment in
test/jit/recompile-getval-demote-retired.js). Therefore every new
test:

- wraps its whole scenario in `function main() { ... } main();`, with
  every helper a LOCAL function declaration inside `main`;
- passes the function under test and its data to drivers as
  parameters (`drive(f, arr, n)`), so the hot loops contain no global
  reads;
- keeps `main`'s own ById reads (e.g. `C.prototype`) to a handful, well
  under the threshold, and names them in a comment when counters are
  pinned.

Dump ordering, verified in the code: `-Xdump-jitcode` prints
`JIT compilation of FunctionID <n>, '<f>'[ (version N)]` when a
compile starts, the code (with `//` comments) while it is emitted,
then `JIT successfully compiled FunctionID <n>, '<f>'[ (version N)]`
and the summary lines (`JIT cold ById sites`, `JIT ByVal sites`,
`JIT ById sites`). Checks are written in that order.

---

### Task 2: Recording helper, demotion, counters, dump line

GetById-family sites call `_jit_get_by_id` through their helper slot
and record shapes; legacy single-shape specializations are recorded as
emitted. No tier consumes the records yet. STAGING RULE: do not add
SHAPE-DRIVEN progress in this task (no `byIdShapeProgress`, and the
ById demotion branch treats sites as unable to progress); the existing
cold-ById-warmed progress term stays exactly as it is.

**Files:**
- Modify: `lib/VM/JIT/JitHandlers.h`, `lib/VM/JIT/JitHandlers.cpp`
- Modify: `include/hermes/VM/JIT/JitCounters.h`
- Modify: `lib/VM/JIT/JitCompiler.cpp` (`isRecordingHelper`,
  demotion pass in `considerRecompile`, `demoteAllSitesOnRetirement`,
  dump output ~620-635)
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` (`byIdSiteRecord`,
  `getByIdImpl`, `DECL_GET_BY_ID`)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (`GetByIdImpl`)
- Test: `test/jit/x86-64/getbyid-record-demote.js`,
  `getbyid-record-demote-delay.js`, `getbyid-record-throw.js`

**Interfaces:**
- Consumes: Task 1 types and accessors.
- Produces:
  `SHLegacyValue _jit_get_by_id(SHRuntime *, const SHLegacyValue
  *source, SHSymbolID, SHReadPropertyCacheEntry *, SHJitVersionData *,
  uint32_t siteId)`;
  `bool isEligibleByIdShape(HiddenClass *objClass, HiddenClass
  *protoClass)` (protoClass null for an own entry);
  `bool addByIdShape(JitByIdSiteRecord &site, CompressedPointer objClass,
  CompressedPointer protoClass, uint16_t slot, bool isProto, bool
  emitted)` returning true if the record changed;
  `JitByIdSiteRecord &Emitter::byIdSiteRecord(uint32_t siteId)`;
  `GetByIdImpl::site_` (`JitByIdSiteRecord *`, null when not recording)
  and `GetByIdImpl::recording_` (`bool`);
  `JitCounter::NumByIdDemotions`;
  dump line `JIT ById sites: <r> recorded, <p> polymorphic, <m>
  megamorphic`.

- [ ] **Step 1: Invoke the gc-safe-coding skill**

The helper roots a value and reads heap objects; follow the skill's
rules for everything below.

- [ ] **Step 2: Write the failing tests**

Each file starts with the standard copyright header.

`test/jit/x86-64/getbyid-record-demote.js`:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s

// A GetById site whose traffic is entirely uncacheable (a missing
// property) records no shape but sets `seen`, so the demotion pass
// flips it: one "changed" crossing, then three stable ones -- the flip
// lands at the 4 * 64 = 256th decline; 300 reads clear it. Its read
// cache never warms, so no recompile can happen. main() has no ById
// read of its own; the global body reads `main` once.
function main() {
  function get(o) {
    return o.missing;
  }
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  print(drive(get, {a: 1}, 300));
}
main();

// CHECK: undefined
// CHECK: JIT counters:
// CHECK: NumRecompiles: 0{{$}}
// CHECK: NumByIdDemotions: 1{{$}}

// BC-LABEL: Function<get>({{.*}}
// BC: GetById
```

`test/jit/x86-64/getbyid-record-demote-delay.js` — the ById twin of
test/jit/recompile-getval-demote-delay.js (A/B crossing equality):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-max-recompiles=0 -Xjit-emit-counters %s 2>&1 | %FileCheck %s

// Two sites, identical decline volume (256 each, so 4 crossings each:
// NumRecompileChecks 8), differing only in whether a knowledge
// transition lands inside the stability window. With a budget of 0 no
// shape is ever actionable, so only the `changed` branch can reset
// stability.
//
// getA: 256 missing-property reads: changed at crossing 1, stable at
// 2, 3, 4 -> demoted at the 256th decline.
// getB: the same, except decline 129 (first of the third window) reads
// an existing own property: a new shape, `changed`, so crossing 3
// resets and crossing 4 reaches only 1. Exactly ONE demotion; deleting
// the ById `changed` reset makes this read 2.
function main() {
  function getA(o) {
    return o.p;
  }
  function getB(o) {
    return o.p;
  }
  function drive(f, o, n) {
    for (var i = 0; i < n; ++i)
      f(o);
  }
  drive(getA, {}, 256);
  drive(getB, {}, 128);
  getB({p: 1});
  drive(getB, {}, 127);
  print('done');
}
main();

// CHECK: done
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 8{{$}}
// CHECK: NumByIdDemotions: 1{{$}}
```

`test/jit/x86-64/getbyid-record-throw.js` (Review Focus 4):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck %s

// A recording site whose forwarded lookup throws, 200 times: the
// exception reaches the caller every time, and the throwing calls are
// neither recorded nor counted -- so no threshold crossing happens at
// all. Counting before forwarding would produce 200 / 16 = 12 checks.
function main() {
  var proto = {
    get boom() {
      throw new Error('boom');
    },
  };
  function get(o) {
    return o.boom;
  }
  function drive(f, o, n) {
    var caught = 0;
    for (var i = 0; i < n; ++i) {
      try {
        f(o);
      } catch (e) {
        ++caught;
      }
    }
    return caught;
  }
  print(drive(get, Object.create(proto), 200));
}
main();

// CHECK: 200
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 0{{$}}
// CHECK: NumByIdDemotions: 0{{$}}
```

- [ ] **Step 3: Run them to see them fail**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-record' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all three FAIL (`NumByIdDemotions` is not a counter yet).

- [ ] **Step 4: Add the counter**

In `include/hermes/VM/JIT/JitCounters.h`, append to `JIT_COUNTERS`
(move the trailing `\` accordingly):

```cpp
  /* GetById sites whose helper slot the demotion pass flipped from  \
   * _jit_get_by_id to the plain helper. Retirement-time flips are   \
   * bookkeeping, not policy, and are NOT counted. */                \
  X(NumByIdDemotions)
```

- [ ] **Step 5: Implement the helper and its record logic**

In `lib/VM/JIT/JitHandlers.h`, after `_jit_get_by_index`, declare
(include `hermes/VM/JIT/JitFunctionData.h` if not already reachable,
and forward-declare `class HiddenClass;`):

```cpp
/// \return true if a GetById shape with receiver class \p objClass and,
/// for a prototype entry, prototype class \p protoClass (null for an
/// own entry) satisfies the read cache's own predicates (spec:
/// "Eligibility").
bool isEligibleByIdShape(HiddenClass *objClass, HiddenClass *protoClass);

/// Add one shape to \p site per the spec's recording rule: an equal
/// live entry absorbs it (marking it emitted if \p emitted), else it
/// overwrites a dead entry, else it is appended, else the site becomes
/// megamorphic. \return true if the record changed.
bool addByIdShape(
    JitByIdSiteRecord &site,
    CompressedPointer objClass,
    CompressedPointer protoClass,
    uint16_t slot,
    bool isProto,
    bool emitted);

/// Slow path of GetById and the recording site for its tiers' declines:
/// forwards to _sh_ljs_get_by_id_rjs, records the shape the read cache
/// now describes into \p versionData's record for \p siteId, counts the
/// decline (possibly triggering a recompile), and returns the result.
/// See the 2026-10-14 polymorphic GetById spec, "The recording helper".
SHLegacyValue _jit_get_by_id(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    uint32_t siteId);
```

In `lib/VM/JIT/JitHandlers.cpp` (add includes for
`hermes/VM/HiddenClass.h`, `hermes/VM/PropertyCache.h`,
`hermes/VM/JSObject.h` if missing):

```cpp
bool isEligibleByIdShape(HiddenClass *objClass, HiddenClass *protoClass) {
  if (!protoClass)
    return !objClass->isDictionaryNoCache();
  return !objClass->isDictionary() && !protoClass->isDictionaryNoCache();
}

bool addByIdShape(
    JitByIdSiteRecord &site,
    CompressedPointer objClass,
    CompressedPointer protoClass,
    uint16_t slot,
    bool isProto,
    bool emitted) {
  JitByIdShapeEntry *dead = nullptr;
  for (auto &e : site.entries) {
    if (!e.isLive()) {
      if (!dead)
        dead = &e;
      continue;
    }
    if (e.isProto == isProto && e.objClass == objClass &&
        (!isProto || e.protoClass == protoClass) && e.slot == slot) {
      if (emitted)
        e.emitted = 1;
      return false;
    }
  }
  JitByIdShapeEntry ne;
  ne.objClass = objClass;
  if (isProto)
    ne.protoClass = protoClass;
  ne.slot = slot;
  ne.isProto = isProto;
  ne.emitted = emitted;
  if (dead) {
    // Overwrites every field, including emitted and unemittable.
    *dead = ne;
    return true;
  }
  if (site.entries.size() < JitByIdSiteRecord::kMaxEntries) {
    site.entries.push_back(ne);
    return true;
  }
  if (!site.megamorphic) {
    site.megamorphic = 1;
    return true;
  }
  return false;
}

/// Step 3 of the recording helper: record what \p shEntry now describes
/// for the object in \p source. Allocates nothing on the GC heap.
static void recordByIdObservation(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    const SHLegacyValue *source,
    SHReadPropertyCacheEntry *shEntry) {
  NoAllocScope noAlloc(runtime);
  if (!site.seen) {
    site.seen = 1;
    site.changed = 1;
  }
  HermesValue src = *toPHV(source);
  if (!src.isObject())
    return;
  auto *entry = reinterpret_cast<ReadPropertyCacheEntry *>(shEntry);
  JSObject *obj = vmcast<JSObject>(src);
  CompressedPointer now = obj->getClassGCPtr();
  CompressedPointer clazz = entry->clazz.getNoBarrierUnsafe();
  bool changed;
  if (clazz && clazz == now) {
    // Own hit. Takes precedence over negMatchClazz, which the own fill
    // does not clear (StaticH.cpp, _sh_ljs_get_by_id_rjs).
    if (!isEligibleByIdShape(obj->getClass(runtime), nullptr))
      return;
    changed = addByIdShape(
        site, now, CompressedPointer(nullptr), entry->getSlot(), false,
        false);
  } else if (now && entry->negMatchClazz.getNoBarrierUnsafe() == now) {
    JSObject *parent = obj->getParent(runtime);
    if (!clazz || !parent ||
        CompressedPointer(parent->getClassGCPtr()) != clazz)
      return;
    if (!isEligibleByIdShape(
            obj->getClass(runtime), parent->getClass(runtime)))
      return;
    changed = addByIdShape(site, now, clazz, entry->getSlot(), true, false);
  } else {
    return;
  }
  if (changed)
    site.changed = 1;
}

LLVM_ATTRIBUTE_NOINLINE
SHLegacyValue _jit_get_by_id(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    uint32_t siteId) {
  Runtime &runtime = getRuntime(shr);
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  // Step 1: forward and root the result at once; step 4 can compile and
  // collect.
  struct : public Locals {
    PinnedValue<> result;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SHLegacyValue res =
      _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  lv.result = HermesValue::fromRaw(res.raw);

  // Step 2: staleness. A getter run above may have retired this body,
  // or a demotion pass may have flipped the site.
  JitFunctionData *jd = vd->codeBlock->getJitData();
  if (vd == jd->current.get() && vd->consumerRecords) {
    // The emitter created the record when it emitted this call.
    JitByIdSiteRecord *site = vd->consumerRecords->findByIdSite(siteId);
    if (site && site->helper == (void *)_jit_get_by_id)
      recordByIdObservation(runtime, *site, source, propCacheEntry);
  }

  // Step 4: count; a crossing may recompile (and collect).
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold))
    runtime.getJITContext().considerRecompile(runtime, vd);
  return lv.result.getHermesValue();
}
```

If `vmcast<JSObject>(HermesValue)` or `PinnedValue<>::operator=(HermesValue)`
do not compile as written, use the forms neighboring StaticH.cpp code
uses (`vmcast<JSObject>(src.getObject())`, `lv.result = src;`)
without changing the semantics.

- [ ] **Step 6: Demotion identity, retirement flip, dump line**

In `lib/VM/JIT/JitCompiler.cpp`, extend `isRecordingHelper`:

```cpp
static bool isRecordingHelper(void *h) {
  return h == (void *)_jit_put_by_val_loose ||
      h == (void *)_jit_put_by_val_strict || h == (void *)_jit_get_by_val ||
      h == (void *)_jit_get_by_index || h == (void *)_jit_get_by_id;
}
```

In `demoteAllSitesOnRetirement`, replace the Task 1 ById loop with:

```cpp
  for (auto &s : cr->byIdSites) {
    if (s.helper == (void *)_jit_get_by_id)
      s.helper = (void *)_sh_ljs_get_by_id_rjs;
    // Never read again; dropping the entries means a retired record's
    // weak references need no tracing (spec: "GC").
    s.entries.clear();
  }
```

In `considerRecompile`'s demotion pass, inside
`if (cr->recordingSites != 0) {`, after the ByVal loop:

```cpp
      for (auto &s : cr->byIdSites) {
        if (s.helper != (void *)_jit_get_by_id)
          continue;
        // Staging: no tier consumes ById shapes yet. Task 3 replaces
        // this with byIdSiteCanProgress(s), together with the chain.
        bool canProgress = false;
        if (!s.seen || canProgress || s.changed) {
          s.unchangedCrossings = 0;
        } else if (++s.unchangedCrossings >= kDemotionStableCrossings) {
          s.helper = (void *)_sh_ljs_get_by_id_rjs;
          --cr->recordingSites;
          if (counters_.get())
            ++counters_.get()[(unsigned)JitCounter::NumByIdDemotions];
        }
        s.changed = 0;
      }
```

In the dump block, after the `JIT ByVal sites:` output and inside the
same `if (auto *cr = ...)`:

```cpp
      size_t recorded = 0, poly = 0, mega = 0;
      for (const auto &s : cr->byIdSites) {
        bool live = false;
        for (const auto &e : s.entries)
          live |= e.isLive();
        if (live || s.megamorphic)
          ++recorded;
        poly += s.chainEmitted;
        mega += s.megamorphic;
      }
      if (recorded) {
        llvh::outs() << "JIT ById sites: " << recorded << " recorded, "
                     << poly << " polymorphic, " << mega
                     << " megamorphic\n";
      }
```

- [ ] **Step 7: Route GetById through the helper slot**

In `lib/VM/JIT/x86-64/JitEmitter.h`, next to `byValSiteRecord`:

```cpp
  /// \return the candidate version record's ById site entry for
  /// \p siteId, creating the records and/or entry if absent. The
  /// entry's address is stable (deque) and may be embedded in code.
  JitByIdSiteRecord &byIdSiteRecord(uint32_t siteId) {
    if (!versionData_->consumerRecords)
      versionData_->consumerRecords = std::make_unique<JitConsumerRecords>();
    return versionData_->consumerRecords->findOrCreateByIdSite(siteId);
  }
```

Add `bool recording` as the last parameter of `getByIdImpl` (doc:
"\param recording true for the GetById family: record shapes through
the site's helper slot"), and change the macro so only `getById`
records:

```cpp
#define DECL_GET_BY_ID(methodName, commentStr, shFn, recording)         \
  void methodName(FR frRes, SHSymbolID symID, FR frSource, uint8_t cacheIdx) { \
    getByIdImpl(                                                        \
        frRes, symID, frSource, cacheIdx, commentStr, shFn, #shFn,      \
        recording);                                                     \
  }
  DECL_GET_BY_ID(getById, "getById", _sh_ljs_get_by_id_rjs, true)
  DECL_GET_BY_ID(tryGetById, "tryGetById", _sh_ljs_try_get_by_id_rjs, false)
```

In `JitEmitter-property.cpp`, `GetByIdImpl` gains two members and a
constructor parameter; `Emitter::getByIdImpl` passes the flag through:

```cpp
  /// Whether this instruction belongs to the GetById family, which
  /// records shapes through its site's helper slot.
  bool recording_;
  /// The candidate version's record for this site; null unless
  /// recording_ and the instruction has a property cache entry.
  JitByIdSiteRecord *site_ = nullptr;
```

(the constructor takes `bool recording` last and initializes
`recording_(recording)`). At the top of `run()`, after the comment:

```cpp
    uint32_t siteId = 0;
    if (recording_ && cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
      siteId = (uint32_t)(
          (const char *)_.emittingIP - (const char *)_.codeBlock_->begin());
      site_ = &_.byIdSiteRecord(siteId);
      // Only a never-flipped slot is initialized; a demoted value
      // carried forward stays demoted.
      if (!site_->helper)
        site_->helper = (void *)_jit_get_by_id;
    }
```

Replace the call emission in `run()` (from `a.mov(x86::rdi, xRuntime);`
through `_.callRuntimeWithSavedIP(...)`) with the following, keeping
the existing x86-64 comment about the RIP-relative cache load:

```cpp
    a.mov(x86::rdi, xRuntime);
    _.loadFrameAddr(x86::rsi, frSource);
    a.mov(x86::edx, asmjit::Imm(symID));
    if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
      a.xor_(x86::ecx, x86::ecx);
    } else {
      a.mov(
          x86::rcx,
          x86::qword_ptr(_.roDataLabel_, _.roOfsReadPropertyCachePtr_));
      if (cacheIdx != 0)
        a.add(
            x86::rcx, asmjit::Imm(sizeof(SHReadPropertyCacheEntry) * cacheIdx));
    }
    if (site_) {
      _.loadBits64InGp(x86::r8, (uint64_t)_.versionData_, "JitVersionData");
      a.mov(x86::r9d, asmjit::Imm(siteId));
      // Type-check the recording callee against the call sequence; the
      // demoted _sh_ljs_get_by_id_rjs ignores r8/r9 under SysV.
      using _FnT = SHLegacyValue (*)(
          SHRuntime *,
          const SHLegacyValue *,
          SHSymbolID,
          SHReadPropertyCacheEntry *,
          SHJitVersionData *,
          uint32_t);
      _FnT _fn = _jit_get_by_id;
      (void)_fn;
      _.callRuntimeWithSavedIPIndirect(
          (uint64_t)&site_->helper, "_jit_get_by_id [indirect]");
    } else {
      _.callRuntimeWithSavedIP((void *)shImpl, shImplName);
    }
```

Record today's single-shape specializations as emitted. At the end of
`emitObjectSpecialization`, just before `return true;`:

```cpp
    if (site_) {
      addByIdShape(
          *site_,
          cacheEntry->clazz.getNoBarrierUnsafe(),
          CompressedPointer(nullptr),
          cacheEntry->getSlot(),
          false,
          true);
    }
```

and at the end of `emitParentSpecialization`, before `return true;`:

```cpp
    if (site_) {
      addByIdShape(
          *site_,
          cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
          cacheEntry->clazz.getNoBarrierUnsafe(),
          cacheEntry->getSlot(),
          true,
          true);
    }
```

Both shapes came from the read cache fill, so they satisfy Eligibility
by construction, and both classes hold lazy IDs, so they are pinned.

- [ ] **Step 8: Run the new tests**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-record' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all three PASS.

- [ ] **Step 9: Prove they can fail**

Mutation A: in `recordByIdObservation`, delete `site.seen = 1;` →
rebuild → getbyid-record-demote FAILS (`NumByIdDemotions: 0`).
Mutation B: in the ById demotion loop, drop `|| s.changed` → rebuild
→ getbyid-record-demote-delay FAILS (`NumByIdDemotions: 2`).
Mutation C: move the `++vd->declineCount` block in `_jit_get_by_id`
above the forwarded call → rebuild → getbyid-record-throw FAILS
(`NumRecompileChecks: 12`). Restore each, rebuild, re-run green.

- [ ] **Step 10: Run the JIT suite and reconcile expectations**

```bash
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Three effects of this task are expected to change existing tests:
GetById declines now count toward crossings (notably drivers that read
global functions, e.g. `loadA` from `driveA` in
recompile-getval-demote-delay.js, whose `NumRecompileChecks: 8` pin
will grow); every counters dump gains a `NumByIdDemotions:` line; and
`-Xdump-jitcode` gains a `JIT ById sites:` line wherever a compiled
body recorded a shape (including legacy single-shape
specializations). For each failing test: read it, confirm the failure
is one of these three effects, and fix it so that the test's intent is
preserved — prefer isolating the measured site (the Test isolation
rule above: pass the function as a parameter, wrap in `main`) over
loosening a check; when a pinned number must change, recompute it and
explain it in the test's comment. Any failure not explained by those
effects is a bug: stop and debug it (superpowers:systematic-debugging).
Re-run until green.

- [ ] **Step 11: Commit**

```bash
git add lib/VM/JIT include/hermes/VM/JIT test/jit
git commit   # "JIT: record GetById shapes through a per-site helper slot"
```

---

### Task 3: Polymorphic chain and the progress term

**Files:**
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (`GetByIdImpl`)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (progress term, demotion
  `canProgress`)
- Test: `test/jit/x86-64/getbyid-poly-own.js`,
  `getbyid-poly-proto.js`, `getbyid-poly-mixed.js`,
  `getbyid-poly-mega.js`, `getbyid-poly-monotone.js`,
  `getbyid-poly-generic-join.js`, `getbyid-poly-boundary.js`,
  `getbyid-poly-edge.js`

**Interfaces:**
- Consumes: Task 1 records; Task 2 `addByIdShape`,
  `isEligibleByIdShape`, `site_`, dump line.
- Produces: `static bool byIdSiteCanProgress(const JitByIdSiteRecord &)`,
  `static bool byIdShapeProgress(const JitVersionData *)` in
  JitCompiler.cpp; the code comment `// Get polymorphic chain (N
  entries)`.

- [ ] **Step 1: Write the failing tests**

Standard copyright header on each; Test isolation rule applies.
`-Xdump-jitcode=2` prints banners and summaries, `=3` also code.

`getbyid-poly-own.js`:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s

// Four receiver classes, `x` an own property at a different slot in
// each. Version 1 (compiled cold) records all four through the helper;
// the first crossing finds unemitted live entries and recompiles; the
// chain then serves every read inline, so nothing declines again.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 3]);
    return s;
  }
  var objs = [{x: 1}, {a: 0, x: 2}, {a: 0, b: 0, x: 3}, {a: 0, b: 0, c: 0, x: 4}];
  print(drive(get, objs, 1000));
}
main();

// OUT: 2500
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
```

`getbyid-poly-proto.js` — Richards-style, four distinct (receiver
class, prototype class) pairs covering both relationships: one
receiver class reaching two prototype classes, and two receiver
classes reaching one prototype class:

```js
// RUN: (the three RUN lines of getbyid-poly-own.js)

// new A() and new B() share the empty receiver class (receiver classes
// do not depend on the prototype); PA and PB differ by their extra
// property. new C() ({q}) and new D() ({r}) differ but share the
// prototype object, hence its class. main()'s own ById reads are the
// seven `.prototype` accesses, below the threshold.
function main() {
  function A() {}
  A.prototype.run = function () { return 1; };
  A.prototype.pa = 0;
  function B() {}
  B.prototype.run = function () { return 2; };
  B.prototype.pb = 0;
  function C() { this.q = 0; }
  C.prototype.run = function () { return 3; };
  function D() { this.r = 0; }
  D.prototype = C.prototype;
  function get(t) {
    return t.run;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 3])();
    return s;
  }
  print(drive(get, [new A(), new B(), new C(), new D()], 1000));
}
main();

// OUT: 2250
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
```

`getbyid-poly-mixed.js` — prototype traffic first, then alternating
own shapes at the same site (a stale `negMatchClazz` must not block
the own entries); phase 2 triggers version 3:

```js
// RUN: (the three RUN lines of getbyid-poly-own.js)
function main() {
  function P1() {}
  P1.prototype.x = 10;
  function P2() { this.k = 0; }
  P2.prototype.x = 20;
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  print(drive(get, [new P1(), new P2()], 100));
  print(drive(get, [{x: 1}, {y: 0, x: 2}], 100));
}
main();

// OUT: 1500
// OUT-NEXT: 150
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 3)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 3)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 megamorphic
// CNT: NumRecompiles: 2{{$}}
```

`getbyid-poly-mega.js` — six own shapes: four emitted, overflow keeps
arriving through the stable crossings, and the site still demotes:

```js
// RUN: (the three RUN lines of getbyid-poly-own.js)
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 6]);
    return s;
  }
  var objs = [
    {x: 1}, {a: 0, x: 2}, {b: 0, x: 3},
    {c: 0, x: 4}, {d: 0, x: 5}, {e: 0, x: 6},
  ];
  print(drive(get, objs, 6000));
}
main();

// OUT: 21000
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
```

`getbyid-poly-monotone.js` — version 1 is compiled WARM (threshold
compile, not `force`), so the site gets today's object specialization
for `a`'s class; later shapes make it polymorphic, and version 2's
chain must still contain version 1's shape:

```js
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=8 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=8 -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s

// Sum: 50 reads of `a` (50); then 100 odd iterations read `a` (100)
// and the 100 even ones alternate others[0] and others[1]
// (50 * 2 + 50 * 3 = 250). Total 400.
function main() {
  function get(o) {
    return o.x;
  }
  function run(f, a, others) {
    var s = 0;
    for (var i = 0; i < 50; ++i)
      s += f(a);
    for (var i = 0; i < 200; ++i)
      s += f(i & 1 ? a : others[(i >> 1) & 1]);
    return s;
  }
  print(run(get, {x: 1}, [{p: 0, x: 2}, {q: 0, x: 3}]));
}
main();

// OUT: 400
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get'{{$}}
// DUMP: // Get from object specialization
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get'{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
```

`getbyid-poly-generic-join.js` — after a four-entry chain (own pointer,
own boxed double, prototype string, prototype boxed double) is
installed, late own shapes hit the live-cache generic tier inline;
primitives go to the helper (Review Focus 5). `1.1`/`2.2`/`3.3` are not
representable inline under HV32/BOXED, so they exercise the boxed
decode:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --match-full-lines %s

// Version 2 declines exactly 6 times (first read of each late shape,
// four primitive reads), below the threshold: NumRecompileChecks stays
// at version 1's single crossing. If late reads missed the generic
// tier they would decline 600 times. main()'s own ById reads: four
// `.prototype`, `.tag` once -- below the threshold.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(arr[i % arr.length]);
    return r;
  }
  function Pr() {}
  Pr.prototype.v = 'proto';
  function Pd() { this.k = 0; }
  Pd.prototype.v = 2.2;
  var o1 = {v: {tag: 'obj'}};
  var o2 = {w: 0, v: 1.1};
  var o3 = new Pr();
  var o4 = new Pd();
  drive(get, [o1, o2, o3, o4], 300);
  print(drive(get, [{z: 0, y: 0, v: 3.3}], 300));
  print(drive(get, [{u: 0, v: 'late'}], 300));
  print(get(o1).tag, get(o2), get(o3), get(o4));
  print(get('str'), get(7));
  String.prototype.v = 'sv';
  Number.prototype.v = 42;
  print(get('str'), get(7));
}
main();

// CHECK: 3.3
// CHECK-NEXT: late
// CHECK-NEXT: obj 1.1 proto 2.2
// CHECK-NEXT: undefined undefined
// CHECK-NEXT: sv 42
// CHECK: NumRecompileChecks: 1
// CHECK: NumRecompiles: 1
```

`getbyid-poly-boundary.js` — the first cacheable shape arrives on the
very call that crosses the threshold on which an uncacheable-only site
would demote; it is recorded before the crossing is processed, so the
site recompiles with a CHAIN (the legacy single-shape tier would show
`Get from object specialization` instead, which is what makes this
fail before this task):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s

// Crossing 1 (call 16): `seen` is new, changed. Crossings 2, 3 (calls
// 32, 48): stable 1, 2. Crossing 4 (call 64) would reach 3 and demote;
// call 64 is the first to read an existing `x`.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f) {
    var s = 0;
    for (var i = 1; i <= 64; ++i)
      s += f(i === 64 ? {x: 5} : {}) === undefined ? 0 : 1;
    return s;
  }
  print(drive(get));
}
main();

// CNT: 1
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
```

`getbyid-poly-edge.js` — Review Focus items 1-3:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(arr[i & 3]);
    return r;
  }
  // Out-of-line slots: 12 properties before `x` (more than the direct
  // slots, fewer than the dictionary threshold).
  function mk(prefix, last) {
    var o = {};
    for (var i = 0; i < 12; ++i)
      o[prefix + i] = i;
    o.x = last;
    return o;
  }
  // Prototype entries whose receiver class is the empty-object class.
  function E1() {}
  E1.prototype.x = 'e1';
  function E2() {}
  E2.prototype.x = 'e2';
  var big = mk('p', 'bigown');
  var viaBig = Object.create(mk('q', 'bigproto'));
  var shapes = [new E1(), new E2(), big, viaBig];
  drive(get, shapes, 400);
  print(get(shapes[0]), get(shapes[1]), get(big), get(viaBig));
  // Possibly the same receiver class as `new E1()`, but no parent.
  print(get(Object.create(null)));
  print(get(new Proxy({}, {get: function () { return 'trap'; }})));
}
main();

// Three entries, not four: E1.prototype and E2.prototype acquire the
// same properties in the same order and share a class, so new E1() and
// new E2() are ONE (empty receiver, prototype class) entry -- the chain
// loads from whichever parent the receiver has; Object.create(...) also
// yields the empty receiver class.
// CHECK: e1 e2 bigown bigproto
// CHECK-NEXT: undefined
// CHECK-NEXT: trap
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
```

After the chain exists (Step 5), confirm once with `-Xdump-jitcode=3`
that the chain's `big`/`viaBig` loads go through `propStorage` (the
indirect path); if 12 properties still fit the direct slots on this
build, raise the count (keep it below the dictionary threshold) and
note the numbers in the test comment.

- [ ] **Step 2: Run them to see them fail**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-poly' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: every DUMP check and the CNT checks of own, proto, mixed,
mega and generic-join FAIL (no chain; polymorphic sites keep
declining); boundary's DUMP fails (legacy tier instead of a chain);
OUT checks and edge's CHECK lines may already pass — they pin
semantics.

- [ ] **Step 3: Progress term and demotion `canProgress`**

In `lib/VM/JIT/JitCompiler.cpp`, after `byValShapeProgress`:

```cpp
/// \return true if \p s holds a live shape that the owning body did not
/// emit and that a compile could still emit.
static bool byIdSiteCanProgress(const JitByIdSiteRecord &s) {
  for (const auto &e : s.entries)
    if (e.isLive() && !e.emitted && !e.unemittable)
      return true;
  return false;
}

/// \return true if some GetById site can progress (spec: "Progress
/// term").
static bool byIdShapeProgress(const JitVersionData *vd) {
  const JitConsumerRecords *cr = vd->consumerRecords.get();
  if (!cr)
    return false;
  for (const auto &s : cr->byIdSites)
    if (byIdSiteCanProgress(s))
      return true;
  return false;
}
```

In the ById demotion loop, replace the staging line and its comment
with:

```cpp
        bool canProgress = actionablePossible && byIdSiteCanProgress(s);
```

and after `if (!progress) progress = byValShapeProgress(vd);` add:

```cpp
  if (!progress)
    progress = byIdShapeProgress(vd);
```

- [ ] **Step 4: Emit the chain**

In `GetByIdImpl` add:

```cpp
  /// One chain entry, resolved to lazy JIT IDs.
  struct PolyEntry {
    uint16_t objId;
    uint16_t protoId;
    SlotIndex slot;
    bool isProto;
  };
  /// The entries emitPolyChain() emits, own entries first.
  llvh::SmallVector<PolyEntry, JitByIdSiteRecord::kMaxEntries> poly_;

  /// Fold the shape today's single-shape gate would specialize into the
  /// record, if eligible and absent (subject to the cap).
  void addCacheShape(ReadPropertyCacheEntry *cacheEntry) {
    Runtime &rt = _.runtime_;
    HiddenClass *clazz = cacheEntry->clazz.get(rt, rt.getHeap());
    HiddenClass *neg = cacheEntry->negMatchClazz.get(rt, rt.getHeap());
    if (!clazz)
      return;
    if (!neg) {
      if (isEligibleByIdShape(clazz, nullptr))
        addByIdShape(
            *site_,
            cacheEntry->clazz.getNoBarrierUnsafe(),
            CompressedPointer(nullptr),
            cacheEntry->getSlot(),
            false,
            false);
    } else if (isEligibleByIdShape(neg, clazz)) {
      addByIdShape(
          *site_,
          cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
          cacheEntry->clazz.getNoBarrierUnsafe(),
          cacheEntry->getSlot(),
          true,
          false);
    }
  }

  /// Resolve the site's live entries to lazy JIT IDs before any chain
  /// code is emitted. Every initHCLazyIDMayAlloc() is a GC safepoint,
  /// so each class is re-read from its weak root right before use.
  /// \return true if at least one entry will be emitted.
  bool preparePolyChain(ReadPropertyCacheEntry *cacheEntry) {
    bool anyLive = false;
    for (const auto &e : site_->entries)
      anyLive |= e.isLive();
    if (!anyLive)
      return false;
    if (cacheEntry->numGoodChanges == 1)
      addCacheShape(cacheEntry);
    Runtime &rt = _.runtime_;
    for (auto &e : site_->entries) {
      if (!e.isLive())
        continue;
      uint16_t objId =
          _.initHCLazyIDMayAlloc(e.objClass.get(rt, rt.getHeap()));
      uint16_t protoId = 0;
      if (objId && e.isProto)
        protoId =
            _.initHCLazyIDMayAlloc(e.protoClass.get(rt, rt.getHeap()));
      if (!objId || (e.isProto && !protoId)) {
        e.unemittable = 1;
        continue;
      }
      e.emitted = 1;
      poly_.push_back({objId, protoId, e.slot, (bool)e.isProto});
    }
    if (poly_.empty())
      return false;
    std::stable_partition(
        poly_.begin(), poly_.end(), [](const PolyEntry &p) {
          return !p.isProto;
        });
    site_->chainEmitted = 1;
    return true;
  }

  /// Emit the chain. Entry state: temp1 = object, temp2 = compressed
  /// receiver class. Fallthrough (the generic tier's join): the same.
  /// hwRes may alias a temp; it is written only on hit paths, which
  /// jump to contLab.
  void emitPolyChain(Emit_sh_shv_decode &shvDecode) {
    _.comment("// Get polymorphic chain (%u entries)", (unsigned)poly_.size());
    // temp3 = receiver lazyJITId, decoded without clobbering temp2.
    // Loaded even if every entry is a prototype entry.
    const auto &reg =
        emit_sh_cp_decode_non_null_preserve_input(a, temp3, temp2);
    a.movzx(
        temp3.r32(), x86::word_ptr(reg, RuntimeOffsets::hiddenClassLazyJITId));
    bool anyProto = false;
    for (const PolyEntry &p : poly_) {
      if (p.isProto) {
        anyProto = true;
        continue;
      }
      asmjit::Label next = a.newLabel();
      a.cmp(temp3.r32(), asmjit::Imm(p.objId));
      a.jne(next);
      // May clobber temp1 (indirect slot); this path ends at contLab.
      emitLoadFromSlot(hwRes.gpq(), temp1, p.slot);
      shvDecode.emitFirstCase(a);
      a.jmp(contLab);
      a.bind(next);
    }
    if (!anyProto)
      return;
    asmjit::Label join = a.newLabel();
    // temp4 = parent; temp2 = parent's lazyJITId (scratch from here on).
    emit_load_cp(a, temp4, x86::ptr(temp1, offsetof(SHJSObject, parent)));
    a.test(temp4, temp4);
    a.jz(join);
    emit_sh_cp_decode_non_null(a, temp4);
    emit_load_cp(a, temp2, x86::ptr(temp4, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, temp2);
    a.movzx(
        temp2.r32(), x86::word_ptr(temp2, RuntimeOffsets::hiddenClassLazyJITId));
    for (const PolyEntry &p : poly_) {
      if (!p.isProto)
        continue;
      asmjit::Label next = a.newLabel();
      a.cmp(temp3.r32(), asmjit::Imm(p.objId));
      a.jne(next);
      a.cmp(temp2.r32(), asmjit::Imm(p.protoId));
      a.jne(next);
      // Clobbers temp4 only after both guards succeeded.
      emitLoadFromSlot(hwRes.gpq(), temp4, p.slot);
      shvDecode.emitFirstCase(a);
      a.jmp(contLab);
      a.bind(next);
    }
    a.bind(join);
    // Restore the generic tier's operand.
    emit_load_cp(a, temp2, x86::ptr(temp1, offsetof(SHJSObject, clazz)));
  }
```

In `emitFastPath`, replace the single-shape selection statement (the
`if (ReadPropertyCacheEntry *cacheEntry = ...; numGoodChanges == 1)
{...} else if (...) { coldReadCacheIdxs_ ... }`) with:

```cpp
    ReadPropertyCacheEntry *cacheEntry =
        _.codeBlock_->getReadCacheEntry(cacheIdx);
    if (site_ && preparePolyChain(cacheEntry)) {
      // Replaces the single-shape choice; always followed by the
      // generic tier below. Not a cold site.
      emitPolyChain(shvDecode);
    } else if (cacheEntry->numGoodChanges == 1) {
      // ... the existing object/parent specialization code, unchanged,
      // including the terminal `return` after a parent specialization ...
    } else if (!cacheEntry->clazz.getNoBarrierUnsafe()) {
      _.coldReadCacheIdxs_.push_back(cacheIdx);
    }
```

The generic tier after it is unchanged; its indirect-slot path's
`shvDecode.emitAll(a)` stays the ONLY rest-case emission and is
followed by `a.jmp(contLab)` as today, so the shared tail reaches
`done`, never the slow call. Non-objects already jump to
`slowPathLab` (the helper call) before any tier; keep it that way.

- [ ] **Step 5: Run the tests**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all PASS. Then do the `propStorage` confirmation for
getbyid-poly-edge.js described under its listing.

- [ ] **Step 6: Prove they can fail**

Mutation A: remove the `addByIdShape(..., true)` call in
`emitObjectSpecialization` AND the `addCacheShape` call in
`preparePolyChain` → rebuild → getbyid-poly-monotone FAILS
(`(2 entries)`). Mutation B: in `recordByIdObservation`, require
`!entry->negMatchClazz` for the own hit → getbyid-poly-mixed FAILS.
Mutation C: in `addByIdShape`, return true on every overflow →
getbyid-poly-mega FAILS (`NumByIdDemotions: 0`). Mutation D: in
`_jit_get_by_id`, move the counting block (step 4) above the staleness
check (step 2), so a crossing is processed before the observation that
caused it is recorded → getbyid-poly-boundary FAILS (demotes at call
64, no chain). Mutation E: make `emitPolyChain` skip
the final `temp2` reload → getbyid-poly-generic-join FAILS
(`NumRecompileChecks` grows: late reads miss the generic tier).
Restore each, rebuild, green.

- [ ] **Step 7: Run the JIT suite**

```bash
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: green. Tests that pin legacy `Get from parent
specialization`/`Get from object specialization` at sites that now
get a chain, or exact recompile counts at polymorphic GetById sites,
may change; reconcile exactly as in Task 2 Step 10 (explain every
changed expectation; any unexplained failure is a bug).

- [ ] **Step 8: Commit**

```bash
git add lib/VM/JIT test/jit
git commit   # "JIT: emit a polymorphic GetById chain from recorded shapes"
```

---

### Task 4: Robustness — invalidation, GC, retirement, ID exhaustion

Tests, plus a fix in this task if one exposes a bug (debug it with
superpowers:systematic-debugging; the test becomes the regression
gate).

**Files:**
- Test: `test/jit/x86-64/getbyid-poly-invalidate.js`,
  `getbyid-poly-gc.js`, `getbyid-poly-retire-flip.js`,
  `getbyid-poly-retire-reenter.js`, `getbyid-poly-rooted-obj.js`,
  `getbyid-poly-rooted-str.js`, `getbyid-poly-hcid.js`,
  `getbyid-poly-dict.js`
- Build tree: `cmake-build-x86jit-hsan`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: nothing new.

- [ ] **Step 1: Configure the moving-GC tree**

Hades' handle sanitizer forces a young collection (and starts, and
under some conditions compacts, the old generation), but does not
guarantee that every relevant object -- hidden classes are old-gen --
moves at the allocation under test. MallocGC's sanitizer moves every
live object and updates weak roots at every allocation, so it is the
tree that deterministically exposes an untraced weak root or an
unrooted result:

```bash
cmake -B cmake-build-x86jit-hsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" -DHERMESVM_ALLOW_JIT=2 \
  -DHERMESVM_GCKIND=MALLOC -DHERMESVM_SANITIZE_HANDLES=ON
cmake --build cmake-build-x86jit-hsan --target hermes
```

- [ ] **Step 2: Write the tests**

Standard copyright header on each; Test isolation rule applies.

`getbyid-poly-invalidate.js` — after a chain with own and prototype
entries is installed, each mutation is observed through misses:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
function main() {
  function get(o) {
    return o.m;
  }
  function drive(f, arr, n) {
    for (var i = 0; i < n; ++i)
      f(arr[i % 3]);
  }
  function P() {}
  P.prototype.m = 'proto';
  function Q() { this.k = 0; }
  Q.prototype.m = 'qproto';
  var p = new P(), q = new Q(), own = {m: 'own'};
  drive(get, [p, q, own], 300);
  print(get(p), get(q), get(own));
  // 1. Shadowing: p gains an own `m` (new receiver class).
  p.m = 'shadow';
  print(get(p));
  // 2. Prototype swap without a receiver class change.
  var q2 = new Q();
  Object.setPrototypeOf(q2, {m: 'swapped'});
  print(get(q2));
  // 3. The prototype becomes a dictionary through delete.
  delete Q.prototype.m;
  print(get(new Q()));
  // 4. A data property redefined as a getter on the prototype.
  Object.defineProperty(P.prototype, 'm', {
    get: function () { return 'getter'; },
  });
  print(get(new P()));
  drive(get, [new P(), new Q(), own], 300);
  print(get(new P()), get(new Q()), get(own));
}
main();

// CHECK: proto qproto own
// CHECK-NEXT: shadow
// CHECK-NEXT: swapped
// CHECK-NEXT: undefined
// CHECK-NEXT: getter
// CHECK-NEXT: getter undefined own
```

`getbyid-poly-gc.js` — a recorded, never-emitted prototype entry whose
prototype class dies while the receiver class lives:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck %s
function main() {
  function get(o) {
    return o.uniqueGcProp;
  }
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  // A prototype whose class nothing else uses; 8 reads (< 16): recorded,
  // no crossing, never emitted, so its class is not pinned.
  function phase1(r) {
    Object.setPrototypeOf(r, {uniqueGcPropA: 0, uniqueGcProp: 'p1'});
    drive(get, r, 8);
  }
  var r = {};
  phase1(r);
  Object.setPrototypeOf(r, {uniqueGcPropB: 0, uniqueGcProp: 'p2'});
  gc();
  print(drive(get, r, 100));
}
main();

// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: // Get polymorphic chain (1 entries)
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: p2
```

If this sees `(2 entries)`, the first prototype's class survived the
collection: find what retains it before changing anything (the read
cache's weak root and the record's are the only expected holders). A
legitimate strong retainer means the test must be redesigned to drop
it — the `(1 entries)` assertion (dead prototype entry never emitted,
never misread as an own entry) is the point of this test and must not
be weakened to output-only.

`getbyid-poly-retire-flip.js` — retirement flips the old body's slot:
a long activation of version 1 keeps running after version 2 is
installed (no OSR) and must stop counting:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck %s

// sum()'s GetById site sits in its own loop. Its first crossing (16th
// iteration) records two unemitted shapes, recompiles, and retires the
// running version 1, flipping its slot to the plain helper; the
// remaining 984 iterations run version 1 without counting. Without the
// flip they would cross 61 more times.
function main() {
  function sum(arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += arr[i & 1].x;
    return s;
  }
  print(sum([{x: 1}, {y: 0, x: 2}], 1000));
}
main();

// CHECK: 1500
// CHECK: NumRecompileChecks: 1{{$}}
// CHECK: NumRecompiles: 1{{$}}
```

`getbyid-poly-retire-reenter.js` — a getter reached through the
recording helper retires `get`'s running version, then makes the
outer call's receiver cacheable and fills the shared cache for it, so
the outer helper call -- returning into the retired version -- finds a
recordable shape. Only the staleness gate keeps it out of the retired
record:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=2 %s | %FileCheck %s

// Inside the getter: (1) 200 polymorphic reads cross the threshold,
// recompile `get` and retire version 1 (whose helper call is still on
// the stack); (2) an own data property `v` gives `tricky` a new class;
// (3) get(this) fills the shared cache with that own shape. Back in
// version 1's helper, the cache now describes the receiver's current
// class. With the gate, nothing is recorded into the retired record;
// without it, the Debug invariant in markWeakElementsInCaches fires at
// gc(). `marker` keeps tricky's class unique.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  var shapes = [{v: 1}, {a: 0, v: 2}];
  var reentered = false;
  var proto = {
    get v() {
      if (!reentered) {
        reentered = true;
        drive(get, shapes, 200);
        Object.defineProperty(this, 'v', {
          value: 7,
          writable: true,
          enumerable: true,
          configurable: true,
        });
        get(this);
      }
      return 100;
    },
  };
  var tricky = Object.create(proto);
  tricky.marker = 1;
  var s = get(tricky) + drive(get, shapes, 10) + get(tricky);
  gc();
  print(s);
}
main();

// 100 (outer getter call) + 15 + 7 (now an own data property).
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: 122
```

`getbyid-poly-rooted-obj.js` and `getbyid-poly-rooted-str.js` —
a pointer-valued result on the crossing call whose recompile
allocates. Each file is its own process with exactly one getter and no
other ById read, so that getter's version-2 compile is the process's
first lazy-ID assignment and creates `usedHCs` -- an allocation, which
on `cmake-build-x86jit-hsan` moves every live object, including the
result the helper holds. (Boxed doubles are not pointer results here:
a boxed slot value is unboxed into the returned HermesValue; their
decode is covered by getbyid-poly-generic-join.js.)

```js
// getbyid-poly-rooted-obj.js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=4 %s | %FileCheck --match-full-lines %s
// The fourth call is the crossing; it returns {n: 4}.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, out, n) {
    for (var i = 0; i < n; ++i)
      out[i] = f(arr[i & 3]);
  }
  var objs = [{v: {n: 1}}, {a: 0, v: {n: 2}}, {b: 0, v: {n: 3}}, {c: 0, v: {n: 4}}];
  var o = [];
  drive(get, objs, o, 64);
  print(o[3].n, o[63].n);
}
main();

// CHECK: 4 4
```

```js
// getbyid-poly-rooted-str.js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=4 %s | %FileCheck --match-full-lines %s
// The fourth call is the crossing; it returns a heap string.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, out, n) {
    for (var i = 0; i < n; ++i)
      out[i] = f(arr[i & 3]);
  }
  // Heap strings, not literals.
  function str(n) {
    return 's' + n;
  }
  var strs = [{v: str(1)}, {a: 0, v: str(2)}, {b: 0, v: str(3)}, {c: 0, v: str(4)}];
  var s = [];
  drive(get, strs, s, 64);
  print(s[3], s[63]);
}
main();

// CHECK: s4 s4
```

Verify the allocation claim once per file: temporarily add
`llvh::errs() << "usedHCs created\n";` right after the successful
`ArrayStorageSmall::create` in `Emitter::initHCLazyIDMayAlloc`, run each
file on `cmake-build-x86jit` with `-Xdump-jitcode=2`, and see the line
between `get`'s `(version 2)` compilation banner and its success
banner; then remove the print.

`getbyid-poly-hcid.js` — the lazy ID space is exhausted:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-hc-id-limit=1 -Xjit-emit-counters %s 2>&1 | %FileCheck %s

// One ID exists. Version 2 emits one entry and marks the other two
// unemittable, so no further progress: exactly one recompile, and the
// site then demotes on the stable-crossing schedule.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 3]);
    return s;
  }
  print(drive(get, [{x: 1}, {a: 0, x: 2}, {b: 0, x: 3}], 3000));
}
main();

// CHECK: 6000
// CHECK: NumRecompiles: 1{{$}}
// CHECK: NumByIdDemotions: 1{{$}}
```

`getbyid-poly-dict.js` — an own entry on a cacheable dictionary
receiver class is emitted in the chain (the DUMP check: two entries,
`d`'s and `plain`'s); a later delete gives the receiver a new class
and is read correctly:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    for (var i = 0; i < n; ++i)
      f(arr[i & 1]);
  }
  var d = {x: 'dict', t: 0};
  delete d.t;  // cacheable dictionary
  var plain = {y: 0, x: 'plain'};
  drive(get, [d, plain], 200);
  print(get(d), get(plain));
  delete d.x;
  print(get(d));
  d.x = 'again';
  print(get(d));
}
main();

// CHECK: dict plain
// CHECK-NEXT: undefined
// CHECK-NEXT: again
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (2 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
```

- [ ] **Step 3: Run them on both trees**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-' cmake --build cmake-build-x86jit --target check-hermes
LIT_FILTER='getbyid-' cmake --build cmake-build-x86jit-hsan --target check-hermes
```

Expected: PASS on both. On the hsan tree, a new test that lit marks
unsupported (`handle_san`/`gc_malloc` gates) must not be one of
getbyid-poly-gc, -rooted-obj, -rooted-str or -retire-reenter: those
are the reason this tree exists. A failure is either a test bug (fix the test, explain) or
a feature bug (debug systematically, fix here, keep the test).

- [ ] **Step 4: Prove the GC, rooting and retirement tests can fail**

On `cmake-build-x86jit-hsan`, one at a time, rebuilding after each
change and after each restore:
- Mutation A: remove `markById(jd->compiling);` →
  getbyid-poly-rooted-obj and -rooted-str (four entries each, so the
  entries after the first are read after the collection) report an
  ASan error or a wrong value.
- Mutation B: in `_jit_get_by_id`, return `res` instead of the pinned
  value → getbyid-poly-rooted-obj AND getbyid-poly-rooted-str each
  report an ASan error or a wrong value (both must fail; one failing
  does not cover the other).
- Mutation C: in `demoteAllSitesOnRetirement`, drop the ById slot flip
  → getbyid-poly-retire-flip FAILS (`NumRecompileChecks: 62`).
- Mutation D: in `_jit_get_by_id`, remove the WHOLE recording gate
  (both the current-version and the helper-identity checks; record
  whenever the record exists) → getbyid-poly-retire-reenter hits the
  Debug invariant assertion at `gc()`.
- Mutation E: in `isEligibleByIdShape`, reject every dictionary class
  → getbyid-poly-dict's DUMP check FAILS (`(1 entries)`).
If A or B does not fail, the test is not collecting during the
compile: redo the allocation verification above and fix the test
before continuing. Restore everything, rebuild, green.

- [ ] **Step 5: Commit**

```bash
git add test/jit lib/VM/JIT
git commit   # "JIT: robustness tests for the polymorphic GetById chain"
```

---

### Task 5: Matrix, measurements, docs

**Files:**
- Modify: `doc/JIT.md` ("Inline caches", "Recompilation")
- Modify: `doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md`
  (append "Delivered")
- Create: a dz issue for arm64 parity

**Interfaces:**
- Consumes: the finished feature.
- Produces: measured numbers, docs.

- [ ] **Step 1: Full suite on the reference tree**

```bash
cmake --build cmake-build-x86jit --target check-hermes
```

Expected: everything passes (non-JIT tests included).

- [ ] **Step 2: The JIT matrix**

Configure (doc/JITTesting.md configs 3-5, plus malloc and hsan from
Global Constraints) and run the JIT suite on each:

```bash
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed \
         cmake-build-x86jit-rel cmake-build-x86jit-malloc \
         cmake-build-x86jit-hsan; do
  cmake --build $t --target hermes &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no `FAIL`. Tests gated `UNSUPPORTED: gc_malloc` /
`handle_san` are skipped by lit as before; a new test that cannot run
on a configuration gets the same kind of gate with a comment saying
why.

- [ ] **Step 3: Record sizes**

Add a temporary `llvh::errs() << sizeof(JitByIdSiteRecord)` probe on
the HV64 and HV32 trees, note both numbers, remove the probe. Report
the deque overhead separately: the toolchain's standard library
(libstdc++ for these clang builds) allocates `std::deque` storage in
512-byte blocks holding `max(1, 512 / sizeof(T))` elements plus a map
of block pointers; state the per-site cost for a function with 1, 8
and 64 ById sites.

- [ ] **Step 4: Benchmarks**

On a Release JIT tree (`cmake-build-release` style:
`-DCMAKE_BUILD_TYPE=Release -DHERMESVM_ALLOW_JIT=1`), build `hermes`
and `hermesc` at this branch's head and at the parent of Task 1's
commit (a separate worktree). Precompile each benchmark with the
matching `hermesc -O -emit-binary`, then, pinned and serialized,
median of 5:

```bash
for b in richards deltablue raytrace typescript earley-boyer; do
  for i in 1 2 3 4 5; do
    taskset -c 2 <base>/bin/hermes -Xjit base-$b.hbc
    taskset -c 2 <head>/bin/hermes -Xjit head-$b.hbc
  done
done
```

Also confirm with a perf-prof build (`-DCMAKE_CXX_FLAGS=-DHERMES_ENABLE_PERF_PROF`,
`-Xperf-prof`, `perf inject --jit`) that Richards spends ~0% in
`_sh_ljs_get_by_id_rjs` / `getNamedDescriptorUnsafe`.

Expected: Richards +8-10%; DeltaBlue and RayTrace up; nothing worse
than noise. Results that miss expectations are reported as measured,
with the profile, not tuned away.

- [ ] **Step 5: Docs**

doc/JIT.md "Inline caches": add the polymorphic chain as a tier (when
it is emitted, entry layout, register join state, one decoder per
site). "Recompilation": GetById now records and triggers through
`_jit_get_by_id`; the ById progress term; `NumByIdDemotions`; the
`JIT ById sites` dump line; the `compiling` pointer. Append to the
spec a "Delivered (date)" section with the matrix results, sizes and
benchmark table.

- [ ] **Step 6: File the arm64 follow-up**

```bash
dz add "JIT: arm64 parity for the polymorphic GetById tier" --type task \
  --component jit -m - --json <<'EOF'
The x86-64 polymorphic GetById chain (spec
doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md)
has no arm64 emitter yet: arm64 GetById still calls
_sh_ljs_get_by_id_rjs directly and records nothing. Port the helper
slot call, legacy-specialization recording and emitPolyChain to
lib/VM/JIT/arm64/JitEmitter-property.cpp; the shared records,
handler, progress term and demotion already work for arm64.
EOF
```

- [ ] **Step 7: Commit**

```bash
git add doc/JIT.md doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md dz/issues
git commit   # "JIT: document the polymorphic GetById tier"
```

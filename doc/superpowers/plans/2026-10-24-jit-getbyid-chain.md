# JIT GetById Chain Entries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inline GetById hits for data properties two to eight
prototypes up (the DeltaBlue `inheritsFrom` shape) on the x86-64 and
arm64 JITs, by recording epoch-validated chain entries next to the
polymorphic tier's own and prototype entries.

**Architecture:** A chain entry is (receiver class, depth, slot, epoch).
A non-allocating walk (`jitWalkChain`) finds and validates the holder;
the recording helper records chain entries when the read cache yields
nothing, flagging every object on the path so that any change to it
bumps the runtime's parent cache epoch, which is widened to 64 bits so
it never wraps. Emitted code checks the receiver class and the epoch,
then follows the live chain; misses refresh stale epochs (a fast
refresh before the forwarded lookup, a revalidation after it), also
after demotion through a new `_jit_get_by_id_refresh` helper.

**Tech Stack:** C++17, asmjit x86-64 and a64, lit/FileCheck (arm64 under
qemu-user), GoogleTest.

**Spec:** doc/superpowers/specs/2026-10-24-jit-getbyid-chain-design.md
(Codex-reviewed over 4 rounds; binding). It extends
doc/superpowers/specs/2026-10-14-jit-getbyid-polymorphic-design.md (the
"poly spec"). Read both in full before Task 1, and read
`DESIGN-PRINCIPLES.md` (repo root). Where this plan resolves an
ambiguity in the spec it says so in the task ("Spec resolution"); the
list is also in the last section.

## Global Constraints

- Scope: x86-64 and arm64. Only `GetById`, `GetByIdShort` and
  `GetByIdLong` record (all reach `Emitter::getById`). Non-goals, from
  the spec: accessors at any depth, missing properties, primitive
  receivers, `GetByIdWithReceiver*`, `TryGetById*`, chain entries for
  depth 1 or beyond `kMaxChainDepth`.
- `JitByIdShapeEntry::kMaxChainDepth` = 8. Found requires depth >= 2
  and `desc.slot <= ReadPropertyCacheEntry::kMaxSlot` (0xffff).
  `JitByIdSiteRecord::kMaxEntries` stays 4.
- `Runtime::parentCacheEpoch_` becomes `uint64_t`; the add cache's
  readers keep calling `getParentCacheEpoch()` and see exactly today's
  24-bit sequence.
- New counters are appended to `JIT_COUNTERS` after `NumByIdDemotions`,
  in this order: `NumByIdChainRefreshes`, `NumByIdChainHits`.
- The dump summary becomes exactly
  `JIT ById sites: R recorded, P polymorphic, C chain, M megamorphic`.
- No bytecode or BytecodeVersion.h changes.
- Every C++ change under `lib/VM/`, `include/hermes/VM/` or
  `unittests/VMRuntime/` is written after invoking the
  `gc-safe-coding` skill. Hermes throws by longjmp: no `LocalsRAII` or
  `GCScope` may span a call that can throw. Never compare raw pointers
  across a GC point.
- 80 columns, 2-space indent, a doc comment on every declaration, only
  trivial one-line methods inside a class body.
- Never bare `cd`; use absolute paths or `( cd dir; cmd; )`.
- Build trees (already configured): `cmake-build-x86jit` (HV64 ASan
  Debug, `HERMESVM_ALLOW_JIT=2`, the reference tree),
  `cmake-build-x86jit-hv32`, `cmake-build-x86jit-boxed`,
  `cmake-build-x86jit-hsan` (MallocGC + `HERMESVM_SANITIZE_HANDLES`),
  `cmake-build-host` (host `hermesc`/`shermes` for the cross builds),
  `cmake-build-arm64`, `cmake-build-arm64-hv32`,
  `cmake-build-arm64-boxed` (qemu-user), `cmake-build-jit2` (Release,
  JIT=2) and `cmake-build-release` (Release, JIT=1).
- Before building ANY arm64 tree, rebuild the host compilers:
  `cmake --build cmake-build-host --target hermesc shermes`.
- JIT suite on a tree:
  `LIT_FILTER='jit/' cmake --build <tree> --target check-hermes`.
  Full suite: the same without `LIT_FILTER`. Rebuild a tree
  (`--target hermes`, plus `HermesVMRuntimeTests` when unit tests
  changed) before running its suite and after every mutation and
  restore. arm64 sanity: `./aarch64/qemu-sanity.sh <tree>` (run from the
  repo root, which is the shell's working directory).
- Unit tests: `cmake --build cmake-build-x86jit --target
  HermesVMRuntimeTests` then
  `cmake-build-x86jit/unittests/VMRuntime/HermesVMRuntimeTests
  --gtest_filter='<pattern>'`. They also run inside `check-hermes`.
- Prove-can-fail: every new test is shown to FAIL with the code it pins
  disabled (the mutation named in the task), then the code is restored,
  the tree REBUILT, and the test re-run green.
- `S` below is your session scratchpad directory (the harness gives its
  path); put every temporary file there, never in the repo.
- Commits: one per task, message per the tightening-commit-messages
  skill, ending with exactly:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MxvtDWnnS16sfyuVjoNLwC
  ```
  Stage by explicit path only (never `git add -A`, never
  `git add dz/issues`). Never push. Never touch the untracked
  `2026-09-08-putbyval-typed-array-measurements.md` and
  `dz/issues/01a063df-0527-71dc-89c2-2464bff4a1e4.md`.
- Reporting: each task's report lists the commit hash, the trees built,
  every suite run with its pass/unsupported/fail counts, and each
  mutation with the failure it produced. Task 9 copies these into the
  spec's "Delivered" section.
- The spec and this plan are committed before Task 1. Before Task 1,
  record `git rev-parse HEAD` as BASE in Task 1's report; Task 9
  benchmarks against BASE.

## Review Focus

Inputs the spec implies but its test list does not name, with the
behavior a person would expect; each has a test in the owning task:

1. A chain holder whose slot is out of line (slot >= 6, so the load goes
   through `propStorage`, which clobbers the object register): must read
   the right value (Task 5, getbyid-chain-depth.js, depth 3, slot 7).
2. A chain value that is a boxed double on HV32 and BOXED builds
   (`emitFirstCase` jumps to the generic tier's rest cases, which clobber
   x86 `r11`): must read the right number (Task 5,
   getbyid-chain-depth.js, value 1.1, run on the hv32 and boxed trees).
3. A pointer-valued result on a call whose revalidation or recording
   flags the path, which allocates: under a moving GC the result must
   survive (Task 7, getbyid-chain-rooted.js, on `cmake-build-x86jit-hsan`).
4. A Proxy receiver and a null-prototype receiver at a site with an
   emitted chain entry: their classes match no entry, so the helper
   returns the trap's value and `undefined` (Task 5,
   getbyid-chain-exclude.js).
5. Lazy JIT ID exhaustion with two chain shapes: one entry emitted, the
   other `unemittable`, exactly one recompile, and the site demotes to
   `_jit_get_by_id_refresh` (Task 5, getbyid-chain-hcid.js).

---

## File Structure

- `include/hermes/VM/Runtime.h` — 64-bit epoch, `getFullParentCacheEpoch`.
- `lib/VM/JIT/RuntimeOffsets.h` — `runtimeParentCacheEpoch`.
- `include/hermes/VM/JIT/JitFunctionData.h` — `JitByIdEntryKind`, the
  widened `JitByIdShapeEntry`, `JitByIdSiteRecord::chainEntryEmitted`.
- `include/hermes/VM/JIT/JitCounters.h` — the two counters.
- `include/hermes/VM/JIT/{x86-64,arm64}/JIT.h` —
  `JITContext::incrementCounter`.
- `lib/VM/JIT/JitHandlers.{h,cpp}` — `addByIdShape`, `jitWalkChain`,
  recording, fast refresh, revalidation, `_jit_get_by_id_refresh`.
- `lib/VM/JIT/JitCompiler.cpp` — carry-forward, retirement, demotion,
  progress, dump line.
- `lib/VM/CodeBlock.cpp` — the retired-record debug check.
- `lib/VM/JIT/{x86-64,arm64}/JitEmitter-property.cpp` — `GetByIdImpl`.
- `unittests/VMRuntime/ObjectModelTest.cpp`,
  `unittests/VMRuntime/JitChainWalkTest.cpp` (new),
  `unittests/VMRuntime/CMakeLists.txt`.
- `test/jit/getbyid-chain*.js` (new) and the seven files whose
  `JIT ById sites` lines change.
- `doc/JIT.md`, the spec's "Delivered" section, dz `01a13df8-970b`.

---

### Test isolation rule (applies to every new lit test)

Process-wide counters (`NumRecompiles`, `NumRecompileChecks`,
`NumByIdDemotions`, `NumByIdChainRefreshes`, `NumByIdChainHits`) see
every function. A declared global read inside a function is a recording
GetById site, and every PutById and PutByVal miss counts a decline of
its own function. Therefore every new test:

- wraps its scenario in `function main() { ... } main();`, with every
  helper a LOCAL function declaration inside `main`;
- passes the function under test and its data to drivers as parameters
  (`drive(f, o, n)`), so the hot loops contain no global reads;
- keeps each function's own decline count (GetById, PutById and
  PutByVal misses) well under 16, and names those reads in a comment
  when counters are pinned; uses `arr[i] = x` rather than `arr.push(x)`
  (a `push` read is a prototype hit that declines on every call of a
  cold body).

Dump ordering, verified in JitCompiler.cpp:559-566 and :685-728:
`-Xdump-jitcode` prints `JIT compilation of FunctionID <n>,
'<f>'[ (version N)]` when a compile starts, the code with its `//`
comments while it is emitted, then `JIT successfully compiled
FunctionID <n>, '<f>'[ (version N)]` and the summary lines
(`JIT cold ById sites`, `JIT ByVal sites`, `JIT ById sites`). Checks
are written in that order. Counters go to stderr after
`JIT counters:` (lib/ConsoleHost/ConsoleHost.cpp:1131-1134), in
`JIT_COUNTERS` order, so counter RUN lines use `2>&1`.

Every new lit test starts with the standard JS copyright header (the
`/** ... */` block of test/jit/count.js:1-6), carries `// REQUIRES: jit`,
and, if it pins counters or dump lines, `// UNSUPPORTED: handle_san`
(as test/jit/getbyid-poly-own.js:11-12 does).

---

### Task 1: Widen the parent cache epoch to 64 bits

**Files:**
- Modify: `include/hermes/VM/Runtime.h:691-732` (accessors),
  `:1455-1469` (fields)
- Modify: `lib/VM/JIT/RuntimeOffsets.h:382-384`
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp:806` (static_assert
  before `class ... Emitter::GetByIdImpl`)
- Test: `unittests/VMRuntime/ObjectModelTest.cpp:888-920`

**Interfaces:**
- Produces: `uint64_t Runtime::getFullParentCacheEpoch() const`;
  `uint32_t Runtime::getParentCacheEpoch() const` (low 24 bits);
  `void Runtime::testSetParentCacheEpoch(uint64_t)` (UNIT_TEST);
  `static constexpr uint32_t RuntimeOffsets::runtimeParentCacheEpoch`.

Today the epoch is `uint32_t` (Runtime.h:1464) and wraps to 1 when it
reaches `AddPropertyCacheEntry::kMaxParentEpoch` (Runtime.h:721-732,
PropertyCache.h:148). Its only readers are the add cache
(JSObject.cpp:3073, Interpreter.cpp:2079, StaticH.cpp:812,
JitHandlers.cpp:391), the array fast path (Array.cpp:454, :540) and
ObjectModelTest.cpp:888-920; writers are JSObject.cpp:314 and
JSObject.h:1458 (`incParentCacheEpoch`).

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Extend the unit test**

In `unittests/VMRuntime/ObjectModelTest.cpp`, at the end of
`TEST_F(ObjectModelTest, ParentCacheEpochTest)` (after line 919,
`EXPECT_FALSE(runtime.checkArrayFastPathParentEpoch());`), add:

```cpp
  // The full counter did not wrap: it went from kMaxParentEpoch - 1
  // straight to kMaxParentEpoch + 2, whose low 24 bits are 1.
  EXPECT_EQ(
      (uint64_t)AddPropertyCacheEntry::kMaxParentEpoch + 2,
      runtime.getFullParentCacheEpoch());
  // The array fast path compares full counters.
  runtime.setArrayFastPathParentEpoch();
  EXPECT_TRUE(runtime.checkArrayFastPathParentEpoch());
  EXPECT_EQ(2, runtime.incParentCacheEpoch());
  EXPECT_FALSE(runtime.checkArrayFastPathParentEpoch());
```

The existing lines 897-919 must stay unchanged and keep passing.

- [ ] **Step 3: See it fail**

```bash
cmake --build cmake-build-x86jit --target HermesVMRuntimeTests
```

Expected: compile error, `getFullParentCacheEpoch` is not a member of
`Runtime`.

- [ ] **Step 4: Widen the epoch**

In `include/hermes/VM/Runtime.h`, replace lines 691-694
(`getParentCacheEpoch`) with:

```cpp
  /// \return the low 24 bits of the parent cache epoch: the part an
  /// AddPropertyCacheEntry stores. Never 0.
  uint32_t getParentCacheEpoch() const {
    return parentCacheEpoch_ & AddPropertyCacheEntry::kMaxParentEpoch;
  }

  /// \return the full parent cache epoch. It only increases and, being
  /// 64-bit, never wraps in practice; GetById chain entries compare it.
  uint64_t getFullParentCacheEpoch() const {
    return parentCacheEpoch_;
  }
```

Leave `checkArrayFastPathParentEpoch` and `setArrayFastPathParentEpoch`
(lines 696-708) as they are, except the assert message, which becomes
`"parent cache epoch went backwards"`. Replace lines 710-732
(`testSetParentCacheEpoch` and `incParentCacheEpoch`) with:

```cpp
#ifdef UNIT_TEST
  /// Unsafe function to use for testing the overflow mechanism. Sets the
  /// full counter.
  void testSetParentCacheEpoch(uint64_t epoch) {
    parentCacheEpoch_ = epoch;
  }
#endif

  /// Increment the parent cache epoch. When its low 24 bits reach
  /// kMaxParentEpoch, invalidate ALL PutById cache entries that have
  /// stored an epoch (i.e. represent a HiddenClass transition), and skip
  /// ahead so the low bits restart at 1 (0 is never a valid add cache
  /// epoch). The full counter keeps increasing.
  /// \return the new low 24 bits, as getParentCacheEpoch() returns them.
  uint32_t incParentCacheEpoch() {
    ++parentCacheEpoch_;
    if (LLVM_UNLIKELY(
            (parentCacheEpoch_ & AddPropertyCacheEntry::kMaxParentEpoch) ==
            AddPropertyCacheEntry::kMaxParentEpoch)) {
      invalidateAllAddCacheEntries();
      parentCacheEpoch_ += 2;
    }
    return getParentCacheEpoch();
  }
```

Replace the two fields at lines 1455-1469 with:

```cpp
  /// An epoch used to invalidate property write cache entries and GetById
  /// chain entries. Incremented whenever a cached object's parent changes
  /// its parent or its HiddenClass, because those operations could
  /// introduce parent chain changes that prevent adding the property the
  /// way that was cached.
  ///
  /// 64-bit and monotonic, so it never wraps in practice. The add cache
  /// stores only its low 24 bits (getParentCacheEpoch()); when those wrap,
  /// ALL property write caches are invalidated. Starts at 1, and the low
  /// bits are never 0, so 0 can be used to ensure an add cache check
  /// fails.
  uint64_t parentCacheEpoch_ = 1;

  /// The full epoch when the array prototype was verified to allow us to
  /// use fast paths in array JSLib functions. If parentCacheEpoch_ is
  /// different, we must reverify the prototype chain.
  uint64_t arrayFastPathParentCacheEpoch_ = 0;
```

Trace (the old ObjectModelTest sequence): set to kMax - 2; the next
increment yields kMax - 1 (low bits kMax - 1, no wrap); the next makes
the low bits kMax, invalidates, and adds 2: full kMax + 2, low bits 1,
which is what the old code returned. `checkArrayFastPathParentEpoch`
compares 1 with kMax + 2: false, as before.

- [ ] **Step 5: Offsets**

In `lib/VM/JIT/RuntimeOffsets.h`, after `runtimeJitCounters`
(line 383), add:

```cpp

  /// The full 64-bit parent cache epoch. Emitted GetById chain entries
  /// compare it with the epoch stored in their record entry.
  static constexpr uint32_t runtimeParentCacheEpoch =
      offsetof(Runtime, parentCacheEpoch_);
  static_assert(
      sizeof(Runtime::parentCacheEpoch_) == 8,
      "chain entries compare the full 64-bit parent cache epoch");
```

(`Runtime` declares `friend struct RuntimeOffsets;`, Runtime.h:1336.)

In `lib/VM/JIT/arm64/JitEmitter-property.cpp`, at namespace scope
immediately before `class HERMES_ATTRIBUTE_INTERNAL_LINKAGE
Emitter::GetByIdImpl {` (line 807), add (the pattern of
arm64/JitEmitter-internal.cpp:506-519):

```cpp
// GetById chain entries load the full parent cache epoch with LDR's
// unsigned-offset form, whose immediate is a 12-bit multiple of 8. The
// offset is an offsetof() into Runtime, which grows: pin it so that
// outgrowing the encoding breaks the build rather than every compile.
static_assert(
    RuntimeOffsets::runtimeParentCacheEpoch % 8 == 0 &&
        RuntimeOffsets::runtimeParentCacheEpoch < kMaxInlineBaseOffset,
    "the parent cache epoch must be reachable by an unsigned-offset LDR");
```

- [ ] **Step 6: Build and run**

```bash
cmake --build cmake-build-x86jit --target hermes HermesVMRuntimeTests
cmake-build-x86jit/unittests/VMRuntime/HermesVMRuntimeTests \
  --gtest_filter='ObjectModelTest.ParentCacheEpochTest'
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
```

Expected: the test passes; the arm64 tree builds. If the arm64
static_assert fires, STOP and report the offset (the spec requires the
inline form; do not work around it).

- [ ] **Step 7: Prove it can fail**

Mutation: in `incParentCacheEpoch`, replace `parentCacheEpoch_ += 2;`
with `parentCacheEpoch_ = 1;` (the old reset) → rebuild → the test
FAILS (`getFullParentCacheEpoch()` is 1, not kMax + 2). Restore,
rebuild, green.

- [ ] **Step 8: Full suite**

```bash
cmake --build cmake-build-x86jit --target check-hermes
```

Expected: everything passes (the interpreter's add cache and the array
fast path depend on the epoch).

- [ ] **Step 9: Commit**

```bash
git add include/hermes/VM/Runtime.h lib/VM/JIT/RuntimeOffsets.h \
  lib/VM/JIT/arm64/JitEmitter-property.cpp \
  unittests/VMRuntime/ObjectModelTest.cpp
git commit   # "VM: widen the parent cache epoch to 64 bits"
```

---

### Task 2: Entry kinds and chain fields in the ById records

A refactor with no behavior change: `isProto` becomes an explicit kind,
entries gain the chain fields, `addByIdShape` takes a whole entry and
never reuses an emitted one, and retirement keeps entries (only their
weak roots go). The gates are the existing suites and a jit-dump
comparison in which only the expected helper-slot displacement
changes (Step 7).

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h:106-181`
- Modify: `lib/VM/JIT/JitHandlers.h:247-257`,
  `lib/VM/JIT/JitHandlers.cpp:546-630`
- Modify: `lib/VM/JIT/JitCompiler.cpp:146-154` (carry-forward),
  `:383-389` (retirement)
- Modify: `lib/VM/CodeBlock.cpp:352-379`
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp:734-746`,
  `:1055-1115`, `:1129-1174`, `:1214-1222`, `:1293-1301`
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp:829-841`,
  `:1148-1208`, `:1222-1265`, `:1300-1308`, `:1371-1379`

**Interfaces:**
- Produces: `enum class JitByIdEntryKind : uint8_t { Own, Proto, Chain }`;
  `JitByIdShapeEntry` fields `objClass`, `protoClass`, `uint64_t epoch`,
  `uint64_t refreshFailedAt`, `uint16_t slot`, `uint8_t depth`,
  `JitByIdEntryKind kind`, `uint8_t emitted`, `uint8_t unemittable`;
  `static constexpr uint8_t JitByIdShapeEntry::kMaxChainDepth = 8`;
  `static JitByIdShapeEntry JitByIdShapeEntry::makeOwn(CompressedPointer
  objClass, uint16_t slot, bool emitted)`;
  `static JitByIdShapeEntry JitByIdShapeEntry::makeProto(CompressedPointer
  objClass, CompressedPointer protoClass, uint16_t slot, bool emitted)`;
  `static JitByIdShapeEntry JitByIdShapeEntry::makeChain(CompressedPointer
  objClass, uint8_t depth, uint16_t slot, uint64_t epoch)`;
  `bool JitByIdShapeEntry::isLive() const`;
  `bool JitByIdShapeEntry::sameIdentity(const JitByIdShapeEntry &) const`;
  `bool addByIdShape(JitByIdSiteRecord &site, const JitByIdShapeEntry &ne)`;
  emitter `PolyEntry { uint16_t objId; uint16_t protoId; SlotIndex slot;
  JitByIdEntryKind kind; }`.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Capture the emitted code before the change**

```bash
cmake --build cmake-build-x86jit --target hermes
mkdir -p $S/chain
cp cmake-build-x86jit/bin/hermes $S/chain/hermes-before-task2
```

- [ ] **Step 3: The record types**

In `include/hermes/VM/JIT/JitFunctionData.h`, replace lines 106-151
(the doc comment and `struct JitByIdShapeEntry`) with:

```cpp
/// What a JitByIdShapeEntry describes. Set when the entry is recorded and
/// never changed, so a cleared weak root never changes what an entry
/// means (DESIGN-PRINCIPLES.md: encode kinds explicitly, not by nullness).
enum class JitByIdEntryKind : uint8_t {
  /// A data property of the receiver itself.
  Own,
  /// A data property of the receiver's immediate prototype, whose class
  /// is protoClass.
  Proto,
  /// A data property `depth` prototypes up (2..kMaxChainDepth), valid
  /// while the runtime's full parent cache epoch equals `epoch`. See the
  /// 2026-10-24 GetById chain spec.
  Chain,
};

/// One observed GetById shape: a receiver class, what kind of lookup
/// answers it, and the slot to load. See the 2026-10-14 polymorphic
/// GetById spec, "Records", and the 2026-10-24 chain spec, "Records".
struct JitByIdShapeEntry {
  /// The deepest prototype, counted from the receiver, that a chain entry
  /// may reach.
  static constexpr uint8_t kMaxChainDepth = 8;

  /// The receiver's hidden class.
  WeakRoot<HiddenClass> objClass{nullptr};
  /// Proto only: the immediate prototype's hidden class.
  WeakRoot<HiddenClass> protoClass{nullptr};
  /// Chain only: the full parent cache epoch at which a walk last
  /// validated this entry. Emitted code reads it through its address,
  /// and the helpers refresh it in place.
  uint64_t epoch = 0;
  /// Chain only: the epoch at which revalidating this entry last failed
  /// for a semantic reason; refreshes skip the entry until the epoch
  /// moves. Reset on carry-forward.
  uint64_t refreshFailedAt = 0;
  /// Slot in the receiver (Own), the prototype (Proto) or the object
  /// `depth` levels up (Chain).
  uint16_t slot = 0;
  /// Chain only: how many prototypes up the property lives,
  /// 2..kMaxChainDepth.
  uint8_t depth = 0;
  /// Set when recorded, never changed.
  JitByIdEntryKind kind = JitByIdEntryKind::Own;
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
    epoch = other.epoch;
    refreshFailedAt = other.refreshFailedAt;
    slot = other.slot;
    depth = other.depth;
    kind = other.kind;
    emitted = other.emitted;
    unemittable = other.unemittable;
    return *this;
  }

  /// \return an own entry for receiver class \p objClass.
  static JitByIdShapeEntry
  makeOwn(CompressedPointer objClass, uint16_t slot, bool emitted);
  /// \return a prototype entry for receiver class \p objClass whose
  /// immediate prototype has class \p protoClass.
  static JitByIdShapeEntry makeProto(
      CompressedPointer objClass,
      CompressedPointer protoClass,
      uint16_t slot,
      bool emitted);
  /// \return an unemitted chain entry for receiver class \p objClass,
  /// validated at \p epoch.
  static JitByIdShapeEntry makeChain(
      CompressedPointer objClass,
      uint8_t depth,
      uint16_t slot,
      uint64_t epoch);

  /// \return true if every class this entry needs is still alive.
  bool isLive() const {
    return objClass && (kind != JitByIdEntryKind::Proto || protoClass);
  }

  /// \return true if \p other describes the same shape: same kind,
  /// receiver class and slot, plus the prototype class (Proto) or the
  /// depth (Chain). Epochs are not part of identity: receiver class,
  /// depth and slot, validated at an epoch, determine the answer.
  bool sameIdentity(const JitByIdShapeEntry &other) const;
};

inline JitByIdShapeEntry JitByIdShapeEntry::makeOwn(
    CompressedPointer objClass,
    uint16_t slot,
    bool emitted) {
  JitByIdShapeEntry e;
  e.objClass = objClass;
  e.slot = slot;
  e.kind = JitByIdEntryKind::Own;
  e.emitted = emitted;
  return e;
}

inline JitByIdShapeEntry JitByIdShapeEntry::makeProto(
    CompressedPointer objClass,
    CompressedPointer protoClass,
    uint16_t slot,
    bool emitted) {
  JitByIdShapeEntry e;
  e.objClass = objClass;
  e.protoClass = protoClass;
  e.slot = slot;
  e.kind = JitByIdEntryKind::Proto;
  e.emitted = emitted;
  return e;
}

inline JitByIdShapeEntry JitByIdShapeEntry::makeChain(
    CompressedPointer objClass,
    uint8_t depth,
    uint16_t slot,
    uint64_t epoch) {
  JitByIdShapeEntry e;
  e.objClass = objClass;
  e.epoch = epoch;
  e.slot = slot;
  e.depth = depth;
  e.kind = JitByIdEntryKind::Chain;
  return e;
}

inline bool JitByIdShapeEntry::sameIdentity(
    const JitByIdShapeEntry &other) const {
  if (kind != other.kind || objClass != other.objClass.getNoBarrierUnsafe() ||
      slot != other.slot)
    return false;
  if (kind == JitByIdEntryKind::Proto)
    return protoClass == other.protoClass.getNoBarrierUnsafe();
  if (kind == JitByIdEntryKind::Chain)
    return depth == other.depth;
  return true;
}
```

(`WeakRootBase` exposes `operator==`/`operator!=` against a
`CompressedPointer` and `operator=(CompressedPointer)`, WeakRoot.h:23-60,
:100; comparing two WeakRoots therefore goes through
`getNoBarrierUnsafe()`.)

In `struct JitByIdSiteRecord` (lines 153-181), replace the doc comment
of `entries` (line 164) with:

```cpp
  /// Observed shapes, at most kMaxEntries. They live in the inline
  /// storage, which never reallocates because addByIdShape never exceeds
  /// kMaxEntries, and are never erased, reordered or (once emitted)
  /// overwritten: emitted code embeds the address of a chain entry's
  /// epoch. Dead entries that were never emitted are reused.
```

- [ ] **Step 4: `addByIdShape` and the harvest**

In `lib/VM/JIT/JitHandlers.h`, replace lines 247-257 with:

```cpp
/// Add \p ne to \p site per the recording rule: a live entry with the
/// same identity absorbs it (marking it emitted if \p ne is), else it
/// overwrites a dead entry that was never emitted, else it is appended,
/// else the site becomes megamorphic. \return true if the record changed.
bool addByIdShape(JitByIdSiteRecord &site, const JitByIdShapeEntry &ne);
```

In `lib/VM/JIT/JitHandlers.cpp`, replace `addByIdShape` (lines 546-588)
with:

```cpp
bool addByIdShape(JitByIdSiteRecord &site, const JitByIdShapeEntry &ne) {
  JitByIdShapeEntry *dead = nullptr;
  for (auto &e : site.entries) {
    if (!e.isLive()) {
      // A body may embed an emitted entry's address (a chain entry's
      // epoch), so only entries that were never emitted are reused.
      if (!dead && !e.emitted)
        dead = &e;
      continue;
    }
    if (e.sameIdentity(ne)) {
      if (ne.emitted)
        e.emitted = 1;
      return false;
    }
  }
  if (dead) {
    // Overwrites every field, including emitted and unemittable.
    *dead = ne;
    return true;
  }
  if (site.entries.size() < JitByIdSiteRecord::kMaxEntries) {
    site.entries.push_back(ne);
    assert(
        site.entries.capacity() == JitByIdSiteRecord::kMaxEntries &&
        "ById entries must stay in the record's inline storage");
    return true;
  }
  if (!site.megamorphic) {
    site.megamorphic = 1;
    return true;
  }
  return false;
}
```

In `recordByIdObservation`, replace the two calls (lines 615-616 and
624) with:

```cpp
    changed = addByIdShape(
        site, JitByIdShapeEntry::makeOwn(now, entry->getSlot(), false));
```

and

```cpp
    changed = addByIdShape(
        site,
        JitByIdShapeEntry::makeProto(now, clazz, entry->getSlot(), false));
```

Behavior is unchanged: emitted entries never die in a current or
candidate record, because their classes are pinned by lazy IDs
(poly spec "Records"); the only dead emitted entries are the retired
ones of Step 6, which nothing writes.

- [ ] **Step 5: The emitters (x86-64 and arm64, identical edits)**

In both `lib/VM/JIT/x86-64/JitEmitter-property.cpp` and
`lib/VM/JIT/arm64/JitEmitter-property.cpp`:

Replace the `PolyEntry` member (x86 :738-743, arm64 :833-838):

```cpp
  /// One chain entry, resolved to lazy JIT IDs.
  struct PolyEntry {
    /// Lazy JIT ID of the receiver's class.
    uint16_t objId;
    /// Proto only: lazy JIT ID of the prototype's class.
    uint16_t protoId;
    /// Slot to load, in the receiver or in the prototype.
    SlotIndex slot;
    /// Which record entry kind this is.
    JitByIdEntryKind kind;
  };
```

In `addCacheShape` (x86 :1061-1078, arm64 :1154-1171) replace the two
calls with:

```cpp
    if (!neg) {
      if (isEligibleByIdShape(clazz, nullptr))
        addByIdShape(
            *site_,
            JitByIdShapeEntry::makeOwn(
                cacheEntry->clazz.getNoBarrierUnsafe(),
                cacheEntry->getSlot(),
                false));
    } else if (isEligibleByIdShape(neg, clazz)) {
      addByIdShape(
          *site_,
          JitByIdShapeEntry::makeProto(
              cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
              cacheEntry->clazz.getNoBarrierUnsafe(),
              cacheEntry->getSlot(),
              false));
    }
```

In `preparePolyChain` (x86 :1094-1112, arm64 :1187-1205) replace the
loop and the partition with:

```cpp
    for (auto &e : site_->entries) {
      if (!e.isLive())
        continue;
      bool isProto = e.kind == JitByIdEntryKind::Proto;
      uint16_t objId = _.initHCLazyIDMayAlloc(e.objClass.get(rt, rt.getHeap()));
      uint16_t protoId = 0;
      if (objId && isProto)
        protoId = _.initHCLazyIDMayAlloc(e.protoClass.get(rt, rt.getHeap()));
      if (!objId || (isProto && !protoId)) {
        e.unemittable = 1;
        continue;
      }
      e.emitted = 1;
      poly_.push_back({objId, protoId, e.slot, e.kind});
    }
    if (poly_.empty())
      return false;
    std::stable_partition(poly_.begin(), poly_.end(), [](const PolyEntry &p) {
      return p.kind == JitByIdEntryKind::Own;
    });
```

In `emitPolyChain`, replace `if (p.isProto) {` (x86 :1131, arm64 :1224)
with `if (p.kind == JitByIdEntryKind::Proto) {` and `if (!p.isProto)`
(x86 :1162, arm64 :1253) with `if (p.kind != JitByIdEntryKind::Proto)`.

At the end of `emitObjectSpecialization` (x86 :1214-1222, arm64
:1300-1308):

```cpp
    if (site_) {
      addByIdShape(
          *site_,
          JitByIdShapeEntry::makeOwn(
              cacheEntry->clazz.getNoBarrierUnsafe(),
              cacheEntry->getSlot(),
              true));
    }
```

At the end of `emitParentSpecialization` (x86 :1293-1301, arm64
:1371-1379):

```cpp
    if (site_) {
      addByIdShape(
          *site_,
          JitByIdShapeEntry::makeProto(
              cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
              cacheEntry->clazz.getNoBarrierUnsafe(),
              cacheEntry->getSlot(),
              true));
    }
```

`rg -n '(\.|->)isProto\b' include/hermes/VM/JIT lib/VM/JIT` must end
empty (it matches only the removed member; `isPrototypeOf` and the
local `bool isProto` in the loop above are legitimate), and the
`JitByIdShapeEntry` declaration no longer has an `isProto` field.

- [ ] **Step 6: Carry-forward, retirement, the debug check**

In `lib/VM/JIT/JitCompiler.cpp`, in the carry-forward loop (lines
151-152) replace `for (auto &e : r->entries) e.unemittable = 0;` with:

```cpp
          for (auto &e : r->entries) {
            e.unemittable = 0;
            e.refreshFailedAt = 0;
          }
```

In `demoteAllSitesOnRetirement`, replace lines 383-389 with:

```cpp
  for (auto &s : llvh::make_pointee_range(cr->byIdSites)) {
    if (s.helper == (void *)_jit_get_by_id)
      s.helper = (void *)_sh_ljs_get_by_id_rjs;
    // The retired body may still be running, and emitted chain entries
    // read their epochs through the entries' addresses, so the entries
    // stay. Their classes go instead: a retired record is not traced
    // (CodeBlock::markWeakElementsInCaches), and the receiver IDs the body
    // compares are immediates, so nothing reads the classes again.
    for (auto &e : s.entries) {
      e.objClass = CompressedPointer(nullptr);
      e.protoClass = CompressedPointer(nullptr);
    }
  }
```

In `lib/VM/CodeBlock.cpp`, replace lines 352-354 (the comment) with:

```cpp
  // Polymorphic GetById shape records hold weak hidden-class references.
  // Only the current body's records and a compile candidate's are read
  // again; retirement nulls a retired record's classes.
```

and lines 368-379 (the `#ifndef NDEBUG` block) with:

```cpp
#ifndef NDEBUG
    // Retired records are not traced, which is sound only because their
    // entries hold no classes: retirement nulls every weak root, and the
    // recording helper's staleness gate keeps them null (a retired body
    // may still run and read its chain entries' epochs, so the entries
    // themselves stay). Check it on every collection, so a violation
    // fails loudly in Debug builds.
    for (auto &vd : jd->retired) {
      if (!vd->consumerRecords)
        continue;
      for (auto &site : vd->consumerRecords->byIdSites)
        for (auto &e : site->entries)
          assert(
              !e.objClass && !e.protoClass &&
              "retired ById entry holds a class");
    }
#endif
```

test/jit/getbyid-poly-retire-reenter.js relies on this check (its
comment says the Debug invariant fires if the staleness gate is
removed); it still fires, because a repopulated retired entry has a
live `objClass`.

- [ ] **Step 7: Build, compare emitted code, run the suites**

```bash
cmake --build cmake-build-x86jit --target hermes
utils/jit/jit-diff.sh $S/chain/hermes-before-task2 cmake-build-x86jit/bin/hermes
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: the JIT suite passes. `jit-diff.sh` exits 1 for exactly one
reason: each `JitByIdShapeEntry` grows by 16 bytes (two `uint64_t`
fields), so `JitByIdSiteRecord::helper`, which follows four inline
entries, moves by 64 bytes and the indirect slow-path call's
displacement changes (HV64: `[r9+128]` becomes `[r9+192]`; the emitter
embeds it, x86 JitEmitter-property.cpp:854, and jit-canon.sh keeps
displacements, utils/jit/jit-canon.sh:60). Review the diff: every
changed line must be that call's displacement, old to new by the same
delta. Any other difference is a bug: investigate it before going on.

- [ ] **Step 8: Prove the retirement change is pinned**

Mutation: delete the `for (auto &e : s.entries) {...}` loop added to
`demoteAllSitesOnRetirement` → rebuild → the JIT suite FAILS with the
`retired ById entry holds a class` assertion in every test that
recompiles a function with recorded ById shapes (e.g.
getbyid-poly-own.js, getbyid-poly-retire-reenter.js). Restore, rebuild,
green.

- [ ] **Step 9: Full suites**

```bash
cmake --build cmake-build-x86jit --target check-hermes
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes &&
  LIT_FILTER='jit/' cmake --build cmake-build-arm64 --target check-hermes ||
  echo "FAIL arm64"
```

Expected: no failure and no `FAIL` line.

- [ ] **Step 10: Commit**

```bash
git add include/hermes/VM/JIT/JitFunctionData.h lib/VM/JIT/JitHandlers.h \
  lib/VM/JIT/JitHandlers.cpp lib/VM/JIT/JitCompiler.cpp lib/VM/CodeBlock.cpp \
  lib/VM/JIT/x86-64/JitEmitter-property.cpp \
  lib/VM/JIT/arm64/JitEmitter-property.cpp
git commit   # "JIT: give GetById shape entries an explicit kind and chain fields"
```

---

### Task 3: The chain walk

**Files:**
- Modify: `lib/VM/JIT/JitHandlers.h` (after `addByIdShape`),
  `lib/VM/JIT/JitHandlers.cpp` (after `addByIdShape`)
- Create: `unittests/VMRuntime/JitChainWalkTest.cpp`
- Modify: `unittests/VMRuntime/CMakeLists.txt:43` (add the file after
  `InterpreterTest.cpp`)

**Interfaces:**
- Consumes: `JitByIdShapeEntry::kMaxChainDepth` (Task 2).
- Produces:
  ```cpp
  struct JitChainWalk {
    enum class Status : uint8_t { Found, Rejected };
    Status status;
    JSObject *holder;
    SlotIndex slot;
    uint8_t depth;
    bool allFlagged;
  };
  JitChainWalk jitWalkChain(Runtime &runtime, JSObject *obj, SymbolID name);
  ```

The walk performs, for the objects it admits, the lookup
`getNamedDescriptorUnsafe` performs (JSObject.cpp:1028-1098): a class
lookup per level. It uses `HiddenClass::tryFindPropertyFast`
(HiddenClass.h:917-935; None when the class has no map and some
properties) and falls back to `HiddenClass::findPropertyNoAlloc`
(HiddenClass.cpp:365-406), which since adee03e65 takes the newest flags
transition's flags and the slot of the class that added the property.
Maps go missing when an addition moves them to a child
(HiddenClass.cpp:565-568), when a flags transition creates a child
(:693-696), and when `updateClassFlags` creates a mapless child
(:790-797). Exclusions are `tryCacheAddProperty`'s
(JSObject.cpp:3064-3071: dictionaries, proxies, host objects) plus lazy
objects; typed objects never change class or parent and are never
flagged (`setCachedUsingEpoch`, JSObject.cpp:2955-2959), so the walk
counts them as flagged.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Write the failing unit tests**

Create `unittests/VMRuntime/JitChainWalkTest.cpp`:

```cpp
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#include "../../lib/VM/JIT/JitHandlers.h"
#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/JSObject.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

/// A large heap: the typed holder below has 65537 slots.
using JitChainWalkTest = LargeHeapRuntimeTestFixture;

/// \return the symbol for \p s. Its handle stays in the current GCScope,
/// which keeps the symbol alive.
SymbolID sym(Runtime &runtime, const char *s) {
  return **runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef(s));
}

/// Store \p v into \p obj's property \p name, adding it if absent.
void put(Runtime &runtime, Handle<JSObject> obj, SymbolID name, double v) {
  GCScopeMarkerRAII marker{runtime};
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      name,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(v))));
}

/// Make \p obj a dictionary by adding and deleting a property \p t.
void makeDictionary(Runtime &runtime, Handle<JSObject> obj, SymbolID t) {
  put(runtime, obj, t, 0);
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, t));
  ASSERT_TRUE(obj->getClass(runtime)->isDictionary());
}

// The slot limit is the read cache's (ReadPropertyCacheEntry::kMaxSlot,
// 0xffff). Only typed classes can have a non-dictionary slot that large,
// so a typed holder reached at depth 2 has 65535 private slots, `a` at
// 0xffff and `b` at 0x10000. Fails if the limit is missing (b is Found)
// or off by one (a is Rejected).
TEST_F(JitChainWalkTest, TypedHolderSlotLimit) {
  GCScope scope{runtime, "TypedHolderSlotLimit", 128};
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> holder;
    PinnedValue<JSObject> mid;
    PinnedValue<JSObject> recv;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");

  lv.clazz = HiddenClass::createForTypedObject(
      runtime, Runtime::makeNullHandle<JSObject>(), 2);
  for (unsigned i = 0; i < 0xffff; ++i)
    HiddenClass::addNewTypedPrivateProperty(lv.clazz, runtime);
  EXPECT_EQ(
      0xffffu,
      HiddenClass::addNewTypedPublicProperty(lv.clazz, runtime, a, true));
  EXPECT_EQ(
      0x10000u,
      HiddenClass::addNewTypedPublicProperty(lv.clazz, runtime, b, true));
  lv.holder =
      JSObject::create(runtime, Runtime::makeNullHandle<JSObject>(), lv.clazz);
  lv.mid = JSObject::create(runtime, lv.holder);
  lv.recv = JSObject::create(runtime, lv.mid);

  JitChainWalk w = jitWalkChain(runtime, *lv.recv, a);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(*lv.holder, w.holder);
  EXPECT_EQ(0xffffu, w.slot);
  EXPECT_EQ(2, (int)w.depth);
  // mid is not flagged; the typed holder counts as flagged.
  EXPECT_FALSE(w.allFlagged);

  w = jitWalkChain(runtime, *lv.recv, b);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);

  JSObject::setCachedUsingEpoch(lv.mid, runtime);
  w = jitWalkChain(runtime, *lv.recv, a);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_TRUE(w.allFlagged);
}

// A holder whose class has no property map and whose `p` went through a
// flags transition: holder and sibling are {p: 1, q: 2} with p then made
// non-enumerable, which keeps p at slot 0 in a flags-transition child C
// (HiddenClass.cpp:625-698); both share C. Adding r to sibling moves C's
// map to C's new child (HiddenClass.cpp:565-568), so the second walk
// falls back to findPropertyNoAlloc on C. Fails if the fallback is
// missing (Rejected), and with adee03e65's HiddenClass.cpp hunk reverted
// (slot 1, the last slot).
TEST_F(JitChainWalkTest, FlagsTransitionHolderWithoutMap) {
  GCScope scope{runtime, "FlagsTransitionHolderWithoutMap", 128};
  struct : public Locals {
    PinnedValue<JSObject> holder;
    PinnedValue<JSObject> sibling;
    PinnedValue<JSObject> mid;
    PinnedValue<JSObject> recv;
    PinnedValue<HiddenClass> c;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SymbolID p = sym(runtime, "p");
  SymbolID q = sym(runtime, "q");
  SymbolID r = sym(runtime, "r");

  DefinePropertyFlags nonEnum{};
  nonEnum.setEnumerable = 1;
  nonEnum.enumerable = 0;
  lv.holder = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());
  put(runtime, lv.holder, p, 1);
  put(runtime, lv.holder, q, 2);
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      lv.holder, runtime, p, nonEnum, Runtime::getUndefinedValue()));
  lv.sibling = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());
  put(runtime, lv.sibling, p, 1);
  put(runtime, lv.sibling, q, 2);
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      lv.sibling, runtime, p, nonEnum, Runtime::getUndefinedValue()));
  lv.c = lv.holder->getClass(runtime);
  ASSERT_EQ(*lv.c, lv.sibling->getClass(runtime));
  lv.mid = JSObject::create(runtime, lv.holder);
  lv.recv = JSObject::create(runtime, lv.mid);

  JitChainWalk w = jitWalkChain(runtime, *lv.recv, p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(0u, w.slot);

  put(runtime, lv.sibling, r, 3);
  ASSERT_EQ(*lv.c, lv.holder->getClass(runtime));
  w = jitWalkChain(runtime, *lv.recv, p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(0u, w.slot);
  EXPECT_EQ(2, (int)w.depth);
  EXPECT_EQ(*lv.holder, w.holder);
}

// The verdicts besides Found, and what allFlagged reports. o[0] holds p
// and q's absence is tested; o[i] has parent o[i - 1], so p is i
// prototypes up from o[i]. Each EXPECT names the rule it pins.
TEST_F(JitChainWalkTest, Verdicts) {
  GCScope scope{runtime, "Verdicts", 128};
  struct : public Locals {
    PinnedValue<JSObject> o[10];
    PinnedValue<JSObject> dictRecv;
    PinnedValue<JSObject> dictMid;
    PinnedValue<JSObject> viaDict;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SymbolID p = sym(runtime, "p");
  SymbolID q = sym(runtime, "q");
  SymbolID t = sym(runtime, "t");

  lv.o[0] = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());
  put(runtime, lv.o[0], p, 1);
  for (unsigned i = 1; i < 10; ++i)
    lv.o[i] = JSObject::create(runtime, lv.o[i - 1]);

  // Own property: Rejected, decided by the receiver's class alone.
  JitChainWalk w = jitWalkChain(runtime, *lv.o[0], p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_TRUE(w.allFlagged);
  // Depth 1 is the prototype entry's job; o[0] is not flagged yet.
  w = jitWalkChain(runtime, *lv.o[1], p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_FALSE(w.allFlagged);
  // Depth 2 and depth 8 (kMaxChainDepth) are Found.
  w = jitWalkChain(runtime, *lv.o[2], p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(2, (int)w.depth);
  EXPECT_EQ(*lv.o[0], w.holder);
  EXPECT_FALSE(w.allFlagged);
  w = jitWalkChain(runtime, *lv.o[8], p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(8, (int)w.depth);
  // Depth 9: not found within kMaxChainDepth.
  w = jitWalkChain(runtime, *lv.o[9], p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  // Not found at all.
  w = jitWalkChain(runtime, *lv.o[3], q);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);

  // Flag o[1] and o[0]: the depth-2 verdict now depends only on flagged
  // objects. Flagging gives o[0] a new, mapless class; p keeps slot 0.
  JSObject::setCachedUsingEpoch(lv.o[1], runtime);
  JSObject::setCachedUsingEpoch(lv.o[0], runtime);
  w = jitWalkChain(runtime, *lv.o[2], p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(0u, w.slot);
  EXPECT_TRUE(w.allFlagged);

  // A dictionary receiver can gain the property without a class change.
  lv.dictRecv = JSObject::create(runtime, lv.o[1]);
  makeDictionary(runtime, lv.dictRecv, t);
  w = jitWalkChain(runtime, *lv.dictRecv, p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_FALSE(w.allFlagged);

  // A dictionary on the path changes in place.
  lv.dictMid = JSObject::create(runtime, lv.o[0]);
  makeDictionary(runtime, lv.dictMid, t);
  lv.viaDict = JSObject::create(runtime, lv.dictMid);
  w = jitWalkChain(runtime, *lv.viaDict, p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_FALSE(w.allFlagged);
}

} // namespace

#endif // HERMESVM_JIT
```

Add `JitChainWalkTest.cpp` to `RTSources` in
`unittests/VMRuntime/CMakeLists.txt`, on its own line after
`InterpreterTest.cpp` (line 43).

If `JSObject::create(runtime, nullParent, clazz)` asserts for the typed
class, use the pattern of lib/VM/Interpreter-slowpaths.cpp:1884-1897
(the same `createForTypedObject` + `JSObject::create` sequence) and say
what differed; the test's claim (Found at 0xffff, Rejected at 0x10000)
must not change.

- [ ] **Step 3: See them fail**

```bash
cmake --build cmake-build-x86jit --target HermesVMRuntimeTests
```

Expected: compile error, `JitChainWalk` and `jitWalkChain` undeclared.

- [ ] **Step 4: Declare the walk**

In `lib/VM/JIT/JitHandlers.h`, add `#include "hermes/VM/PropertyDescriptor.h"`
and `#include "hermes/VM/SymbolID.h"` to the includes, and after the
`addByIdShape` declaration:

```cpp
/// The result of jitWalkChain(): whether a GetById of a name on an object
/// can be answered by a chain entry, and with what. A POD.
struct JitChainWalk {
  /// The verdict.
  enum class Status : uint8_t {
    /// A data property at depth >= 2, slot <= kMaxSlot.
    Found,
    /// Not cacheable as a chain entry.
    Rejected,
  };
  /// The verdict.
  Status status;
  /// Found only: the object holding the property.
  JSObject *holder;
  /// Found only: the property's slot in holder.
  SlotIndex slot;
  /// Found only: prototypes from the receiver to holder.
  uint8_t depth;
  /// Every object the walk visited (the receiver's prototype up to the
  /// holder, or up to where it stopped) is flagged or typed; false if it
  /// stopped at a dictionary, proxy, host or lazy object. Says whether
  /// the verdict is fixed until the parent cache epoch moves.
  bool allFlagged;
};

/// Walk from \p obj's prototype up to JitByIdShapeEntry::kMaxChainDepth
/// levels looking for \p name, as getNamedDescriptorUnsafe would for the
/// objects it admits (spec: "The walk"). Never allocates.
JitChainWalk jitWalkChain(Runtime &runtime, JSObject *obj, SymbolID name);
```

- [ ] **Step 5: Implement it**

In `lib/VM/JIT/JitHandlers.cpp`, after `addByIdShape`:

```cpp
/// \return true if lookups through an object of class \p clazz are not
/// described by a class that only updateClass changes (spec: "The
/// validity argument", 6 and 7), so a chain path must not contain it.
static bool isChainExcluded(const HiddenClass *clazz) {
  return clazz->isDictionary() || clazz->isLazyObject() ||
      clazz->isHostObject() || clazz->isProxyObject();
}

/// Look \p name up in \p clazz without allocating: in the property map if
/// the class has one, else by findPropertyNoAlloc's walk of the
/// transition chain. \return true and fill \p desc if found.
static bool chainClassLookup(
    Runtime &runtime,
    HiddenClass *clazz,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  OptValue<bool> fast =
      HiddenClass::tryFindPropertyFast(clazz, runtime, name, desc);
  if (fast.hasValue())
    return *fast;
  llvh::Optional<NamedPropertyDescriptor> slow =
      HiddenClass::findPropertyNoAlloc(clazz, runtime, name);
  if (!slow)
    return false;
  desc = *slow;
  return true;
}

JitChainWalk jitWalkChain(Runtime &runtime, JSObject *obj, SymbolID name) {
  NoAllocScope noAlloc(runtime);
  JitChainWalk res{JitChainWalk::Status::Rejected, nullptr, 0, 0, false};
  NamedPropertyDescriptor desc;
  HiddenClass *clazz = obj->getClass(runtime);
  // The receiver must be plain too: a dictionary receiver could gain the
  // property without a class change.
  if (isChainExcluded(clazz))
    return res;
  if (chainClassLookup(runtime, clazz, name, desc)) {
    // An own property: the receiver's class alone decides, and entries
    // are keyed on it.
    res.allFlagged = true;
    return res;
  }
  bool allFlagged = true;
  JSObject *cur = obj;
  for (unsigned depth = 1; depth <= JitByIdShapeEntry::kMaxChainDepth;
       ++depth) {
    cur = cur->getParent(runtime);
    if (!cur)
      break;
    clazz = cur->getClass(runtime);
    if (isChainExcluded(clazz)) {
      allFlagged = false;
      break;
    }
    // Typed objects never change class or parent, so they count as
    // flagged (setCachedUsingEpoch skips them).
    if (!clazz->getIsCachedUsingEpoch() && !clazz->isTyped())
      allFlagged = false;
    if (!chainClassLookup(runtime, clazz, name, desc))
      continue;
    // Found here; depth 1 is the prototype entry's job.
    if (depth >= 2 && !desc.flags.accessor && !desc.flags.hostObject &&
        !desc.flags.proxyObject &&
        desc.slot <= ReadPropertyCacheEntry::kMaxSlot) {
      res.status = JitChainWalk::Status::Found;
      res.holder = cur;
      res.slot = desc.slot;
      res.depth = (uint8_t)depth;
    }
    break;
  }
  res.allFlagged = allFlagged;
  return res;
}
```

- [ ] **Step 6: Run the tests**

```bash
cmake --build cmake-build-x86jit --target HermesVMRuntimeTests
cmake-build-x86jit/unittests/VMRuntime/HermesVMRuntimeTests \
  --gtest_filter='JitChainWalkTest.*'
```

Expected: all three PASS.

- [ ] **Step 7: Prove they can fail**

One at a time, rebuilding `HermesVMRuntimeTests` after each change and
each restore:
- Mutation A: delete `desc.slot <= ReadPropertyCacheEntry::kMaxSlot &&`
  → TypedHolderSlotLimit FAILS (b is Found).
- Mutation B: in `chainClassLookup`, return `false` when `fast` has no
  value instead of calling `findPropertyNoAlloc` →
  FlagsTransitionHolderWithoutMap FAILS (second walk Rejected).
- Mutation C: revert adee03e65's lib/VM/HiddenClass.cpp hunk:
  `git show adee03e65 -- lib/VM/HiddenClass.cpp | git apply -R` →
  FlagsTransitionHolderWithoutMap FAILS (slot 1). Restore with
  `git checkout lib/VM/HiddenClass.cpp`.
- Mutation D: in `isChainExcluded`, drop `clazz->isDictionary() ||` →
  Verdicts FAILS (dictionary receiver or path Found).
- Mutation E: change the loop bound to `depth <
  JitByIdShapeEntry::kMaxChainDepth` → Verdicts FAILS (depth 8
  Rejected).

- [ ] **Step 8: Full unit and JIT suites**

```bash
cmake-build-x86jit/unittests/VMRuntime/HermesVMRuntimeTests
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all pass (the walk has no caller yet).

- [ ] **Step 9: Commit**

```bash
git add lib/VM/JIT/JitHandlers.h lib/VM/JIT/JitHandlers.cpp \
  unittests/VMRuntime/JitChainWalkTest.cpp unittests/VMRuntime/CMakeLists.txt
git commit   # "JIT: add the non-allocating GetById chain walk"
```

---

### Task 4: Record and refresh chain entries

The helpers now record chain entries, refresh their epochs and keep
emitted entries fresh after demotion. No emitter emits a chain entry
yet: both `preparePolyChain`s skip the kind (neither `emitted` nor
`unemittable`, so a later compile can emit it), and
`byIdSiteCanProgress` ignores chain entries, so no recompile is spent
on them. In the interim a site that keeps discovering new chain shapes
sets `changed` (delaying demotion exactly as new own or prototype
shapes do), and the demotion-to-refresh branch never fires because no
entry is emitted. Task 5 removes both interim rules on x86-64.

**Files:**
- Modify: `include/hermes/VM/JIT/JitCounters.h:30-33`
- Modify: `include/hermes/VM/JIT/x86-64/JIT.h:178-179`,
  `include/hermes/VM/JIT/arm64/JIT.h:175-176`
- Modify: `lib/VM/JIT/JitHandlers.h:259-273`,
  `lib/VM/JIT/JitHandlers.cpp:590-668`
- Modify: `lib/VM/JIT/JitCompiler.cpp:312-319`, `:383-385`, `:435-448`
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp:850-851`, the
  `preparePolyChain` loop; `lib/VM/JIT/arm64/JitEmitter-property.cpp:939-940`,
  the `preparePolyChain` loop
- Test: `test/jit/getbyid-chain-record.js`

**Interfaces:**
- Consumes: Task 1 `Runtime::getFullParentCacheEpoch()`; Task 2
  `JitByIdEntryKind`, `makeChain`, `addByIdShape`; Task 3
  `jitWalkChain`, `JitChainWalk`.
- Produces: `JitCounter::NumByIdChainRefreshes`;
  `void JITContext::incrementCounter(JitCounter counter)` (both arches);
  `SHLegacyValue _jit_get_by_id_refresh(SHRuntime *, const SHLegacyValue
  *source, SHSymbolID, SHReadPropertyCacheEntry *, SHJitVersionData *,
  SHJitByIdSiteRecord *)`; in JitCompiler.cpp
  `static bool byIdSiteHasEmittedChain(const JitByIdSiteRecord &)`.

**Spec resolution (fast refresh of an unemitted entry).** The spec's
fast refresh returns the holder's value without forwarding and without
counting a decline whenever a candidate matches. For a recorded but not
yet emitted entry that would hide the site's misses from the recompile
trigger: in a program that bumps the epoch between every two misses no
miss would ever count, so the entry would never be emitted. This plan
refreshes such an entry's epoch (and counts the refresh) but lets the
call continue: forward, harvest, walk, count. Only an emitted entry
completes the call early. getbyid-chain-record.js pins this.

**Spec resolution ("harvested").** `recordByIdObservation` returns true
when its own or prototype branch offered a shape to `addByIdShape`
(whether or not it was new); only then is the chain walk skipped.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Write the failing test**

`test/jit/getbyid-chain-record.js` (copyright header first):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=0 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=0 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Recording and refreshing a chain entry that is never emitted: with a
// recompile budget of 0, `get` keeps its cold version 1 and every read
// goes through _jit_get_by_id. 10 + 3 + 3 = 16 calls, 16 declines, one
// threshold crossing (NumRecompileChecks: 1).
// - Call 1 forwards, the read cache yields nothing (p is two prototypes
//   up), and the walk finds p on an unflagged path: mid and base are
//   flagged and a chain entry is recorded at the current epoch.
// - `unrel.y = 2` bumps the epoch: unrel was flagged by the cached add
//   `kid.k = 1` (JSObject::tryCacheAddProperty). Call 11's fast refresh
//   finds the stale entry, walks a fully flagged path to the same depth
//   and slot, and refreshes it (refresh 1). The entry is not emitted, so
//   the call still forwards and counts its decline; returning early
//   would leave 15 declines and no crossing.
// - setPrototypeOf(mid, ...) bumps again and puts an unflagged object
//   with p at the same slot in base's place. Call 14's fast refresh sees
//   an unflagged path and does nothing; after forwarding, revalidation
//   flags the new object and refreshes the entry (refresh 2).
// main() itself declines a handful of times (Object.create,
// Object.setPrototypeOf, the two adds), far below 16.
// Fails before the feature (no NumByIdChainRefreshes counter).
function main() {
  var base = {p: 5, w: 0};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var unrel = {};
  var kid = Object.create(unrel);
  kid.k = 1;
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  var s1 = drive(get, recv, 10);
  unrel.y = 2;
  var s2 = drive(get, recv, 3);
  Object.setPrototypeOf(mid, {p: 7, z: 0});
  var s3 = drive(get, recv, 3);
  print(s1, s2, s3);
}
main();

// OUT: 50 15 21
// CNT: NumRecompileChecks: 1{{$}}
// CNT: NumRecompiles: 0{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 2{{$}}
```

- [ ] **Step 3: See it fail**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-chain' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: the CNT run FAILS (no `NumByIdChainRefreshes` line). OUT may
pass; it pins semantics.

- [ ] **Step 4: The counter and its accessor**

In `include/hermes/VM/JIT/JitCounters.h`, replace lines 30-33 with
(keep the backslashes aligned with the lines above):

```cpp
  /* GetById sites whose helper slot the demotion pass flipped    \
   * from _jit_get_by_id to the plain helper, or to               \
   * _jit_get_by_id_refresh when the site holds an emitted chain  \
   * entry. Retirement-time flips are bookkeeping, not policy,    \
   * and are NOT counted. */                                      \
  X(NumByIdDemotions)                                             \
  /* Chain entries whose epoch a GetById helper set: fast         \
   * refreshes before the forwarded lookup plus revalidations     \
   * after it. Counted by the helpers, so only with               \
   * -Xjit-emit-counters. */                                      \
  X(NumByIdChainRefreshes)
```

In `include/hermes/VM/JIT/x86-64/JIT.h`, after the `dumpCounters`
declaration (line 179), and in `include/hermes/VM/JIT/arm64/JIT.h`
after line 176, add:

```cpp

  /// Increment \p counter if counters are enabled. For runtime helpers;
  /// emitted code increments counters inline.
  void incrementCounter(JitCounter counter) {
    if (counters_.get())
      ++counters_.get()[(unsigned)counter];
  }
```

- [ ] **Step 5: The helpers**

In `lib/VM/JIT/JitHandlers.h`, after the `_jit_get_by_id` declaration
(line 273), add:

```cpp

/// The helper a demoted GetById site with an emitted chain entry calls
/// instead of _sh_ljs_get_by_id_rjs (spec: "_jit_get_by_id_refresh"):
/// if \p versionData is current, a fast refresh, the forwarded lookup,
/// and a revalidation when a stale chain entry exists; otherwise only
/// the forwarded lookup. Records nothing and counts nothing.
SHLegacyValue _jit_get_by_id_refresh(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    SHJitByIdSiteRecord *siteRecord);
```

Also update the doc comment of `_jit_get_by_id` (lines 259-263): "forwards
to _sh_ljs_get_by_id_rjs, records the shape the read cache now
describes, or a chain entry found by jitWalkChain(), into
\p siteRecord, refreshes stale chain entries, counts the decline
(possibly triggering a recompile), and returns the result. See the
2026-10-14 polymorphic GetById spec, "The recording helper", and the
2026-10-24 chain spec, "Recording and refresh"."

In `lib/VM/JIT/JitHandlers.cpp`, change `recordByIdObservation`
(lines 590-630) to return whether it harvested:

```cpp
/// Step 3 of the recording helper: record what \p shEntry now describes
/// for the object in \p source. Allocates nothing on the GC heap.
/// \return true if the read cache described the receiver as an own or
///   prototype hit whose shape was offered to addByIdShape.
static bool recordByIdObservation(
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
    return false;
  auto *entry = reinterpret_cast<ReadPropertyCacheEntry *>(shEntry);
  JSObject *obj = vmcast<JSObject>(src);
  CompressedPointer now = obj->getClassGCPtr();
  CompressedPointer clazz = entry->clazz.getNoBarrierUnsafe();
  bool changed;
  if (clazz && clazz == now) {
    // Own hit. Takes precedence over negMatchClazz, which the own fill
    // does not clear (StaticH.cpp, _sh_ljs_get_by_id_rjs).
    if (!isEligibleByIdShape(obj->getClass(runtime), nullptr))
      return false;
    changed = addByIdShape(
        site, JitByIdShapeEntry::makeOwn(now, entry->getSlot(), false));
  } else if (now && entry->negMatchClazz.getNoBarrierUnsafe() == now) {
    JSObject *parent = obj->getParent(runtime);
    if (!clazz || !parent ||
        CompressedPointer(parent->getClassGCPtr()) != clazz)
      return false;
    if (!isEligibleByIdShape(obj->getClass(runtime), parent->getClass(runtime)))
      return false;
    changed = addByIdShape(
        site,
        JitByIdShapeEntry::makeProto(now, clazz, entry->getSlot(), false));
  } else {
    return false;
  }
  if (changed)
    site.changed = 1;
  return true;
}
```

Then, still before `_jit_get_by_id`, add:

```cpp
/// \return true if \p e is a chain entry for receiver class \p clazz that
/// a walk at epoch \p now may refresh: live, its epoch stale, and not
/// marked as failing at \p now.
static bool isChainCandidate(
    const JitByIdShapeEntry &e,
    CompressedPointer clazz,
    uint64_t now) {
  return e.kind == JitByIdEntryKind::Chain && e.isLive() &&
      e.objClass == clazz && e.epoch != now && e.refreshFailedAt != now;
}

/// \return true if \p site holds a chain candidate for \p clazz at \p now.
static bool hasChainCandidate(
    const JitByIdSiteRecord &site,
    CompressedPointer clazz,
    uint64_t now) {
  for (const auto &e : site.entries)
    if (isChainCandidate(e, clazz, now))
      return true;
  return false;
}

/// \return true if a new entry would be stored rather than overflow:
/// fewer than kMaxEntries entries, or a dead one that was never emitted.
static bool byIdSiteHasRoom(const JitByIdSiteRecord &site) {
  if (site.entries.size() < JitByIdSiteRecord::kMaxEntries)
    return true;
  for (const auto &e : site.entries)
    if (!e.isLive() && !e.emitted)
      return true;
  return false;
}

/// Flag every unflagged, untyped object from \p recv's prototype down
/// \p depth levels, as tryCacheAddProperty does (JSObject.cpp:3104-3113).
/// Flagging changes only class flags (HiddenClass::markAsCachedUsingEpoch),
/// cannot run JS and cannot throw: the objects on a walked path are not
/// lazy, so setCachedUsingEpoch never materializes. It can allocate and
/// collect.
static void flagChainPath(
    Runtime &runtime,
    Handle<JSObject> recv,
    uint8_t depth) {
  struct : public Locals {
    PinnedValue<JSObject> cur;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.cur = recv->getParent(runtime);
  for (uint8_t i = 0; i < depth && lv.cur.get() != nullptr; ++i) {
    if (!lv.cur->isCachedUsingEpoch(runtime))
      JSObject::setCachedUsingEpoch(lv.cur, runtime);
    lv.cur = lv.cur->getParent(runtime);
  }
}

/// The fast refresh (spec: "Recording and refresh"), before the forwarded
/// lookup. Allocates nothing. If a stale chain entry for the receiver's
/// class names the (depth, slot) a fully flagged walk finds, set its
/// epoch. \return true, with the holder's value in \p out, only if that
/// entry is emitted: the call is then complete. An unemitted entry is
/// refreshed but the call continues, so its miss still counts toward
/// the recompile that emits it.
static bool chainFastRefresh(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    const SHLegacyValue *source,
    SymbolID name,
    HermesValue &out) {
  NoAllocScope noAlloc(runtime);
  HermesValue src = *toPHV(source);
  if (!src.isObject())
    return false;
  JSObject *obj = vmcast<JSObject>(src);
  CompressedPointer clazz = obj->getClassGCPtr();
  uint64_t now = runtime.getFullParentCacheEpoch();
  if (!hasChainCandidate(site, clazz, now))
    return false;
  JitChainWalk w = jitWalkChain(runtime, obj, name);
  if (w.status != JitChainWalk::Status::Found || !w.allFlagged)
    return false;
  for (auto &e : site.entries) {
    if (!isChainCandidate(e, clazz, now) || e.depth != w.depth ||
        e.slot != w.slot)
      continue;
    e.epoch = now;
    runtime.getJITContext().incrementCounter(
        JitCounter::NumByIdChainRefreshes);
    if (!e.emitted)
      return false;
    out = JSObject::getNamedSlotValueUnsafe(w.holder, runtime, w.slot)
              .unboxToHV(runtime);
    return true;
  }
  return false;
}

/// After the forwarded lookup: walk from the rooted receiver \p recv and
/// update \p site's chain entries (spec: "Recording and refresh",
/// revalidation and step 4). Flagging the path can allocate and collect;
/// only the walk's scalars cross it, and the receiver's class is re-read
/// afterwards. Never runs JS and never throws. Revalidation never sets
/// `changed`.
/// \param record whether a new (depth, slot) may be recorded; false for
///   _jit_get_by_id_refresh.
static void chainAfterForward(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    Handle<JSObject> recv,
    SymbolID name,
    bool record) {
  JitChainWalk w = jitWalkChain(runtime, *recv, name);
  bool found = w.status == JitChainWalk::Status::Found;
  uint64_t now = runtime.getFullParentCacheEpoch();
  CompressedPointer clazz = recv->getClassGCPtr();
  // The entry with the walk's identity, if any. Entries never move.
  JitByIdShapeEntry *same = nullptr;
  for (auto &e : site.entries) {
    if (e.kind != JitByIdEntryKind::Chain || !e.isLive() ||
        e.objClass != clazz)
      continue;
    if (found && e.depth == w.depth && e.slot == w.slot) {
      same = &e;
      continue;
    }
    // A different verdict that depends only on flagged objects cannot
    // change without a bump: skip this entry until the epoch moves.
    if (w.allFlagged && isChainCandidate(e, clazz, now))
      e.refreshFailedAt = now;
  }
  if (!found)
    return;
  uint8_t depth = w.depth;
  uint16_t slot = (uint16_t)w.slot;
  if (same) {
    if (same->epoch == now)
      return;
    // Revalidation: flag the path so that any change to it bumps the
    // epoch, then adopt the epoch the walk ran at if nothing bumped it.
    // `same` stays valid: entries never move, and its class is the
    // rooted receiver's, so the GC cannot clear it.
    flagChainPath(runtime, recv, depth);
    if (runtime.getFullParentCacheEpoch() == now) {
      same->epoch = now;
      runtime.getJITContext().incrementCounter(
          JitCounter::NumByIdChainRefreshes);
    }
    return;
  }
  if (!record)
    return;
  if (!byIdSiteHasRoom(site)) {
    // Sets megamorphic (and changed) once; nothing is stored, so nothing
    // needs flagging.
    if (addByIdShape(
            site, JitByIdShapeEntry::makeChain(clazz, depth, slot, now)))
      site.changed = 1;
    return;
  }
  flagChainPath(runtime, recv, depth);
  if (runtime.getFullParentCacheEpoch() != now)
    return;
  if (addByIdShape(
          site,
          JitByIdShapeEntry::makeChain(
              recv->getClassGCPtr(), depth, slot, now)))
    site.changed = 1;
}
```

Replace `_jit_get_by_id` (lines 632-668) with:

```cpp
LLVM_ATTRIBUTE_NOINLINE
SHLegacyValue _jit_get_by_id(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    SHJitByIdSiteRecord *siteRecord) {
  Runtime &runtime = getRuntime(shr);
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  // vd owns the record, and a version record is never freed while its
  // body can run (retirement keeps it), so the pointer stays valid
  // across step 2 even if a getter retires this body.
  auto *site = reinterpret_cast<JitByIdSiteRecord *>(siteRecord);
  SymbolID name = SymbolID::unsafeCreate(symID);
  JitFunctionData *jd = vd->codeBlock->getJitData();

  // Step 0: staleness. A retired body or a demoted slot records and
  // refreshes nothing.
  // Step 1: fast refresh; may complete the call.
  if (vd == jd->current.get() && site->helper == (void *)_jit_get_by_id) {
    HermesValue hv;
    if (chainFastRefresh(runtime, *site, source, name, hv))
      return hv;
  }

  // Step 2: forward. A throw longjmps past this frame, skipping
  // destructors, so no LocalsRAII may be registered across the call: a
  // stale one would leave runtime.vmLocals pointing at a dead frame.
  SHLegacyValue res = _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  // Root the result at once (nothing allocates between the return and
  // here); steps 4 and 5 can flag, compile and collect.
  struct : public Locals {
    PinnedValue<> result;
    PinnedValue<JSObject> recv;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.result = HermesValue::fromRaw(res.raw);

  // Step 3: staleness again (a getter run above may have retired this
  // body, or a demotion pass flipped the site), then harvest.
  if (vd == jd->current.get() && site->helper == (void *)_jit_get_by_id) {
    bool harvested =
        recordByIdObservation(runtime, *site, source, propCacheEntry);
    // Step 4: nothing harvested: walk for a chain entry.
    HermesValue src = *toPHV(source);
    if (!harvested && src.isObject()) {
      lv.recv = vmcast<JSObject>(src);
      chainAfterForward(runtime, *site, lv.recv, name, true);
    }
  }

  // Step 5: count; a crossing may recompile (and collect).
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold))
    runtime.getJITContext().considerRecompile(runtime, vd);
  return lv.result.getHermesValue();
}

LLVM_ATTRIBUTE_NOINLINE
SHLegacyValue _jit_get_by_id_refresh(
    SHRuntime *shr,
    const SHLegacyValue *source,
    SHSymbolID symID,
    SHReadPropertyCacheEntry *propCacheEntry,
    SHJitVersionData *versionData,
    SHJitByIdSiteRecord *siteRecord) {
  Runtime &runtime = getRuntime(shr);
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  auto *site = reinterpret_cast<JitByIdSiteRecord *>(siteRecord);
  SymbolID name = SymbolID::unsafeCreate(symID);
  JitFunctionData *jd = vd->codeBlock->getJitData();
  // Retirement flips this slot to the plain helper, so a retired body
  // gets here only for a call already in flight; it only forwards.
  if (vd != jd->current.get())
    return _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  {
    HermesValue hv;
    if (chainFastRefresh(runtime, *site, source, name, hv))
      return hv;
  }
  // Same longjmp rule as _jit_get_by_id.
  SHLegacyValue res = _sh_ljs_get_by_id_rjs(shr, source, symID, propCacheEntry);
  struct : public Locals {
    PinnedValue<> result;
    PinnedValue<JSObject> recv;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.result = HermesValue::fromRaw(res.raw);
  HermesValue src = *toPHV(source);
  if (vd == jd->current.get() && src.isObject()) {
    lv.recv = vmcast<JSObject>(src);
    if (hasChainCandidate(
            *site,
            lv.recv->getClassGCPtr(),
            runtime.getFullParentCacheEpoch()))
      chainAfterForward(runtime, *site, lv.recv, name, false);
  }
  return lv.result.getHermesValue();
}
```

Safepoints, per DESIGN-PRINCIPLES.md 2: `chainFastRefresh` and
`jitWalkChain` run under `NoAllocScope`. After the forwarded call the
result and the receiver are rooted; `flagChainPath` may collect, and
`chainAfterForward` carries only `depth`, `slot`, `now` and the entry
pointer (malloc memory) across it, re-reading the receiver's class
afterwards. `considerRecompile` may compile and collect, as before.

- [ ] **Step 6: Demotion, retirement, interim progress**

In `lib/VM/JIT/JitCompiler.cpp`, after `byIdShapeProgress`
(line 331), add:

```cpp

/// \return true if the owning body emitted a chain entry at \p s.
static bool byIdSiteHasEmittedChain(const JitByIdSiteRecord &s) {
  for (const auto &e : s.entries)
    if (e.kind == JitByIdEntryKind::Chain && e.emitted)
      return true;
  return false;
}
```

In `byIdSiteCanProgress` (lines 314-319) make the loop body:

```cpp
  for (const auto &e : s.entries) {
    // Chain entries are not emitted yet, so they cannot be progress.
    if (e.kind == JitByIdEntryKind::Chain)
      continue;
    if (e.isLive() && !e.emitted && !e.unemittable)
      return true;
  }
```

In the ById demotion branch, replace
`s.helper = (void *)_sh_ljs_get_by_id_rjs;` (line 442) with:

```cpp
          // A site with an emitted chain entry keeps a helper that
          // refreshes it; otherwise one epoch bump after demotion would
          // disable the entry for good.
          s.helper = byIdSiteHasEmittedChain(s)
              ? (void *)_jit_get_by_id_refresh
              : (void *)_sh_ljs_get_by_id_rjs;
```

In `demoteAllSitesOnRetirement`, replace
`if (s.helper == (void *)_jit_get_by_id)` with:

```cpp
    if (s.helper == (void *)_jit_get_by_id ||
        s.helper == (void *)_jit_get_by_id_refresh)
```

`isRecordingHelper` (lines 342-346) is unchanged: the refresh helper is
not a recording helper, so install (lines 637-644) does not count it in
`recordingSites`.

- [ ] **Step 7: Emitters skip chain entries; type-check the new helper**

In both `preparePolyChain` loops (x86 and arm64), right after
`if (!e.isLive()) continue;`, add:

```cpp
      // Chain entries are not emitted yet: no lazy ID, not emitted, and
      // not unemittable either, so a later compile can still emit them.
      if (e.kind == JitByIdEntryKind::Chain)
        continue;
```

In both `run()` methods, after `_FnT _fn = _jit_get_by_id; (void)_fn;`
(x86 :850-851, arm64 :939-940), add:

```cpp
      // Demotion may store _jit_get_by_id_refresh in the slot instead.
      _FnT _fnRefresh = _jit_get_by_id_refresh;
      (void)_fnRefresh;
```

- [ ] **Step 8: Run the test and the JIT suite**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-chain' cmake --build cmake-build-x86jit --target check-hermes
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
utils/jit/jit-diff.sh $S/chain/hermes-before-task2 cmake-build-x86jit/bin/hermes
```

Expected: getbyid-chain-record.js PASSES; the JIT suite passes with the
same counts as after Task 3 plus one. `jit-diff.sh` (which runs with
`-Xjit-max-recompiles=0`, utils/jit/jit-dump.sh:167, so no recompile
can occur) must show only the helper-slot displacement change already
reviewed in Task 2 Step 7 (no chain entry is emitted yet); any other
difference is a bug to investigate. (If `$S/chain/hermes-before-task2`
no longer exists, skip the comparison and say so in the report.) An existing test that changes is
one with a GetById site reading a property two or more prototypes up:
chain recording sets `changed` and flagging changes prototype classes,
which can shift a crossing. For each such failure: confirm the cause
with `-Xdump-jitcode=3` and the counters, fix the test so its intent is
preserved (prefer isolating the measured site per the Test isolation
rule over loosening a check), and explain any changed number in the
test's comment. A failure not explained this way is a bug: stop and
debug it (superpowers:systematic-debugging).

- [ ] **Step 9: Prove the test can fail**

- Mutation A: in `chainFastRefresh`, delete the line
  `if (!e.emitted) return false;`, so every refreshed candidate
  completes the call → rebuild → getbyid-chain-record.js FAILS
  (`NumRecompileChecks: 0`: 15 declines).
- Mutation B: in `chainAfterForward`, make the stale-duplicate branch
  return before it flags the path (so neither flagging, the epoch
  update nor the counter increment happens) → getbyid-chain-record.js
  FAILS (`NumByIdChainRefreshes: 1`). Deleting only the
  `same->epoch = now;` line is not this mutation: flagging still
  happens and the next call's fast refresh revives the entry, giving
  3 refreshes; if you try it, expect that.
Restore, rebuild, green.

- [ ] **Step 10: arm64 and the full x86 suite**

```bash
cmake --build cmake-build-x86jit --target check-hermes
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes &&
  LIT_FILTER='jit/' cmake --build cmake-build-arm64 --target check-hermes ||
  echo "FAIL arm64"
```

Expected: everything passes, no `FAIL` line.

- [ ] **Step 11: Commit**

```bash
git add include/hermes/VM/JIT/JitCounters.h include/hermes/VM/JIT/x86-64/JIT.h \
  include/hermes/VM/JIT/arm64/JIT.h lib/VM/JIT/JitHandlers.h \
  lib/VM/JIT/JitHandlers.cpp lib/VM/JIT/JitCompiler.cpp \
  lib/VM/JIT/x86-64/JitEmitter-property.cpp \
  lib/VM/JIT/arm64/JitEmitter-property.cpp test/jit/getbyid-chain-record.js
git commit   # "JIT: record and refresh GetById chain entries"
```

---

### Task 5: x86-64 chain emission, hit counter, dump line

**Files:**
- Modify: `include/hermes/VM/JIT/JitCounters.h` (after
  `NumByIdChainRefreshes`)
- Modify: `include/hermes/VM/JIT/JitFunctionData.h` (`JitByIdSiteRecord`,
  after `chainEmitted`, line 177)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (carry-forward reset,
  `byIdSiteCanProgress`, dump at :712-727)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (`PolyEntry`,
  `preparePolyChain`, `emitPolyChain`, new `emitChainEntry`)
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp` (`preparePolyChain`:
  interim `unemittable`)
- Modify: the `JIT ById sites` lines of test/jit/getbyid-poly-mixed.js:57,
  getbyid-poly-mega.js:38, getbyid-poly-own.js:39,
  getbyid-poly-proto.js:47, count.js:56 and :67,
  recompile-phased-warming.js:48 and :69, x86-64/exceptions.js:385 and
  :393
- Test: `test/jit/getbyid-chain.js`, `getbyid-chain-depth.js`,
  `getbyid-chain-invalidate.js`, `getbyid-chain-mega.js`,
  `getbyid-chain-exclude.js`, `getbyid-chain-hcid.js`

**Interfaces:**
- Consumes: Task 1 `RuntimeOffsets::runtimeParentCacheEpoch`; Task 2
  entry fields; Task 4 recording.
- Produces: `JitCounter::NumByIdChainHits`;
  `uint8_t JitByIdSiteRecord::chainEntryEmitted`; x86 `PolyEntry
  { uint16_t objId; uint16_t protoId; SlotIndex slot; JitByIdEntryKind
  kind; uint8_t depth; const uint64_t *epochAddr; }`,
  `static unsigned emitRank(JitByIdEntryKind)`,
  `void emitChainEntry(Emit_sh_shv_decode &, const PolyEntry &)`; dump
  line `JIT ById sites: R recorded, P polymorphic, C chain, M
  megamorphic`.

The emitted check (spec "Emission"): entry state `temp1` object, `temp2`
compressed receiver class, `temp3` receiver lazy ID
(JitEmitter-property.cpp:1125-1128). A chain entry compares the ID, loads
its own `epoch` through `r11` (xScratch, JitEmitter.h:204), compares it
with `[r15 + runtimeParentCacheEpoch]`, and only then counts the hit,
follows `objectParent` from the receiver class and `clazz`/`objectParent`
per further level, and loads the slot. A miss on either `jne` leaves
`temp1`-`temp3` untouched and clobbers only `temp4`, `r11` and EFLAGS, so
the next chain entry, the prototype block (which re-derives `temp4`,
:1149-1160) and the generic tier (which needs `temp1`/`temp2`,
:986-1001) see the poly spec's state. No decode or load helper uses
`r11` (JitEmitter-internal.h:333-339, :382-396, :426-436;
JitEmitter-property.cpp:881-899); `emitRestCases` clobbers it under
boxed doubles (JitEmitter-internal.cpp:92-98), after it is dead;
`emitIncrementCounter` uses it and EFLAGS (JitEmitter.cpp:587-602), where
neither is live. `hwRes` may alias a temp (:940-942); it is written only
on the hit path. Chain entries go after own entries and before the
prototype block, so they leave its state untouched.

**Spec resolution (dump count).** The spec's `C` needs a per-site
emitted-tier flag; this plan adds `JitByIdSiteRecord::chainEntryEmitted`
(reset on carry-forward, like `chainEmitted`).

**Interim (until Task 6).** The new tests run on x86-64 only
(`REQUIRES: jit, jit-arch-x86-64` plus a comment), and the arm64
`preparePolyChain` marks chain entries `unemittable`, so on arm64 a chain
shape costs at most the one recompile x86 also spends, and never loops.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Write the failing tests**

Each file starts with the copyright header. Every file in this task
carries, after its RUN lines:

```js
// REQUIRES: jit, jit-arch-x86-64
// UNSUPPORTED: handle_san
// x86-64 only until the arm64 emitter handles chain entries.
```

The three RUN lines used below ("the standard RUN lines") are:

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
```

Write them out in full in every file.

`test/jit/getbyid-chain.js`:

```js
// The DeltaBlue shape (benchmarks/octane/deltablue.js:439-447): an
// Inheriter instance sits between each subclass prototype and Base's,
// so `kind` is two prototypes up from every receiver, and the three
// receiver classes differ because their prototypes differ.
//
// Version 1 is compiled cold. Its generic tier compares the receiver's
// class with the read cache's, which holds Base.prototype's class, so
// each of the first 16 calls declines; calls 1-3 each record a chain
// entry (one per receiver class). Call 16 crosses the threshold, the
// unemitted entries are progress, and version 2 emits all three. Calls
// 17-300 hit inline: NumByIdChainHits = 300 - 16 = 284. Nothing declines
// afterwards, so nothing can demote. main() reads Base.prototype once
// and inheritsFrom reads parent.prototype three times.
// Fails before the feature: nothing is recorded or emitted for `kind`
// (no chain line in the dump, NumByIdChainHits absent).
function main() {
  function inheritsFrom(child, parent) {
    function Inheriter() {}
    Inheriter.prototype = parent.prototype;
    child.prototype = new Inheriter();
  }
  function Base() {}
  Base.prototype.kind = 1;
  function A() {}
  function B() {}
  function C() {}
  inheritsFrom(A, Base);
  inheritsFrom(B, Base);
  inheritsFrom(C, Base);
  function get(o) {
    return o.kind;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 3]);
    return s;
  }
  print(drive(get, [new A(), new B(), new C()], 300));
}
main();

// OUT: 300
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 0{{$}}
// CNT: NumByIdChainHits: 284{{$}}
```

`test/jit/getbyid-chain-depth.js` (Review Focus 1 and 2):

```js
// One site per holder depth, 100 calls each. A site whose version 1
// records a shape recompiles at its 16th call; its calls 17-100 hit.
// - depth 1: the read cache yields a prototype entry, not a chain entry.
// - depth 2: chain entry; p is 1.1, which does not fit the inline
//   compressed-double encoding (SmallHermesValue.h:323), so on HV32 and
//   BOXED it is a heap BoxedDouble and the load takes emitFirstCase's
//   branch to the shared rest cases (x86 JitEmitter-internal.cpp:24).
//   (2.5 would not: its low 32 bits are zero, so it is stored inline.)
// - depth 3: chain entry; the holder has 7 properties before p, so p's
//   slot (7) is out of line (HERMESVM_DIRECT_PROPERTY_SLOTS is 6).
// - depth 8: chain entry at kMaxChainDepth.
// - depth 9: the walk stops at depth 8 and records nothing; the site
//   never recompiles and demotes at its fourth crossing (call 64).
// NumByIdChainHits = 3 x (100 - 16) = 252. NumRecompiles = 4 (depths 1,
// 2, 3, 8). NumByIdDemotions = 1 (depth 9). mk() declines 13 times in
// total (7 keyed stores, 5 `o.p` adds, one Object.create miss).
// Fails before the feature (0 hits, and depths 2, 3 and 8 demote too),
// and if the depth limit is off by one either way (+84 or -84 hits).
function main() {
  function mk(depth, pad, v) {
    var o = {};
    for (var i = 0; i < pad; ++i)
      o['k' + i] = i;
    o.p = v;
    for (var i = 0; i < depth; ++i)
      o = Object.create(o);
    return o;
  }
  function getD1(o) {
    return o.p;
  }
  function getD2(o) {
    return o.p;
  }
  function getD3(o) {
    return o.p;
  }
  function getD8(o) {
    return o.p;
  }
  function getD9(o) {
    return o.p;
  }
  // Counts the reads that return exactly v, so no floating-point sum is
  // pinned.
  function drive(f, o, n, v) {
    var ok = 0;
    for (var i = 0; i < n; ++i)
      if (f(o) === v)
        ++ok;
    return ok;
  }
  var r1 = mk(1, 0, 1);
  var r2 = mk(2, 0, 1.1);
  var r3 = mk(3, 7, 3);
  var r8 = mk(8, 0, 8);
  var r9 = mk(9, 0, 9);
  print(
      drive(getD1, r1, 100, 1),
      drive(getD2, r2, 100, 1.1),
      drive(getD3, r3, 100, 3),
      drive(getD8, r8, 100, 8),
      drive(getD9, r9, 100, 9));
}
main();

// OUT: 100 100 100 100 100
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getD1' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getD1' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getD8' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getD8' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// DUMP-NOT: 'getD9' (version 2)
// CNT: NumRecompiles: 4{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainHits: 252{{$}}
```

`test/jit/getbyid-chain-invalidate.js`:

```js
// Six mutations of an emitted chain's path, one site each (g1..g6,
// since entries are per site). run() builds a fresh base <- mid <- recv
// chain (p two prototypes up), drives its site 20 times (16 declines in
// version 1, recompile, 4 hits: 24 hits over six cases), mutates, and
// reads twice.
// 1. Shadowing add on mid (a class change of a flagged object, so a
//    bump): the walk finds p at depth 1 and the read cache yields a
//    prototype hit; both reads miss the chain. 10 10.
// 2. setPrototypeOf(mid, {x: 0, p: 20}): p is now at slot 1, a new
//    identity, recorded but not emitted; both reads miss. (Replacing it
//    with p at the same slot is the revalidation case of
//    getbyid-chain-refresh-revalidate.js.) 20 20.
// 3. delete base.p: base becomes a dictionary, which the walk rejects.
//    undefined undefined.
// 4. p redefined as a getter on base: an accessor, rejected. 40 40.
// 5. p made non-writable: a flags transition (a bump) that keeps p's
//    slot; the first read's fast refresh finds the same (depth, slot)
//    on a flagged path and refreshes (the only refresh in this test);
//    the second read hits. 1 1.
// 6. base.p = 60: no class change, no bump; both reads hit. 60 60.
// NumByIdChainHits = 24 + 1 + 2 = 27. NumByIdChainRefreshes = 1.
// Removing the emitted epoch compare makes cases 1-4 read 1.
function main() {
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  function build() {
    var base = {p: 1};
    var mid = Object.create(base);
    return [base, mid, Object.create(mid)];
  }
  function run(g, mutate) {
    var t = build();
    drive(g, t[2], 20);
    mutate(t);
    return g(t[2]) + ' ' + g(t[2]);
  }
  function g1(o) {
    return o.p;
  }
  function g2(o) {
    return o.p;
  }
  function g3(o) {
    return o.p;
  }
  function g4(o) {
    return o.p;
  }
  function g5(o) {
    return o.p;
  }
  function g6(o) {
    return o.p;
  }
  print(run(g1, function (t) { t[1].p = 10; }));
  print(run(g2, function (t) { Object.setPrototypeOf(t[1], {x: 0, p: 20}); }));
  print(run(g3, function (t) { delete t[0].p; }));
  print(run(g4, function (t) {
    Object.defineProperty(t[0], 'p', {get: function () { return 40; }});
  }));
  print(run(g5, function (t) {
    Object.defineProperty(t[0], 'p', {writable: false});
  }));
  print(run(g6, function (t) { t[0].p = 60; }));
}
main();

// OUT: 10 10
// OUT-NEXT: 20 20
// OUT-NEXT: undefined undefined
// OUT-NEXT: 40 40
// OUT-NEXT: 1 1
// OUT-NEXT: 60 60
// CNT: NumRecompiles: 6{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 27{{$}}
```

(This file uses only the OUT and CNT RUN lines.)

`test/jit/getbyid-chain-mega.js`:

```js
// Five receiver classes, p two prototypes up from each, at one site.
// Version 1: calls 1-4 record four chain entries; call 5's shape finds
// no room, so addByIdShape sets megamorphic. Call 16 recompiles and
// version 2 emits the four entries. Of calls 17-1000 (i = 16..999),
// those with i % 5 == 4 are the fifth class (i = 19, 24, ..., 999: 197
// calls); the other 787 hit. The fifth class keeps declining; its
// crossings change nothing (megamorphic is already set), so the third
// demotes the site, to _jit_get_by_id_refresh since it holds emitted
// chain entries.
// Fails before the feature, and if the no-room branch skipped
// addByIdShape (the dump would read 0 megamorphic).
function main() {
  function mk(v) {
    return Object.create(Object.create({p: v}));
  }
  function get(o) {
    return o.p;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 5]);
    return s;
  }
  print(drive(get, [mk(1), mk(2), mk(3), mk(4), mk(5)], 1000));
}
main();

// OUT: 3000
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 1 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainHits: 787{{$}}
```

`test/jit/getbyid-chain-exclude.js` (Review Focus 4):

```js
// Walks that must not record (spec "The walk").
// get(): p two prototypes up from each of a receiver whose prototype is
// a dictionary, one whose prototype is a Proxy, and a dictionary
// receiver. None records a chain entry. The fourth receiver has p as an
// own property; its own entry is what makes version 2 compile at all,
// so its dump line can show `0 chain`.
// getName(): `name` two prototypes up from `plain` gives a chain entry
// (version 2 emits it; calls 17-20 of the first count() hit: the only 4
// chain hits). Then fresh functions whose prototype is mid, so `name`
// is also two up (Function.prototype's own `name` is not on their
// chain), each still lazy when read: their own `name` wins. Then a
// Proxy receiver and a null-prototype receiver, whose classes match no
// entry, read through the helper.
// Fails if the walk admits a dictionary on the path or as the receiver
// (get's dump line would read 1 chain).
function main() {
  function get(o) {
    return o.p;
  }
  function getName(o) {
    return o.name;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % arr.length]);
    return s;
  }
  function count(f, arr, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(arr[i % arr.length]) === want)
        ++c;
    return c;
  }
  var dictMid = Object.create({p: 1});
  dictMid.t = 0;
  delete dictMid.t;
  var viaDict = Object.create(dictMid);
  var viaProxy = Object.create(new Proxy(Object.create({p: 2}), {}));
  var dictRecv = Object.create(Object.create({p: 3}));
  dictRecv.t = 0;
  delete dictRecv.t;
  print(drive(get, [viaDict, viaProxy, dictRecv, {p: 4}], 100));

  var mid = Object.create({name: 'top'});
  print(count(getName, [Object.create(mid)], 20, 'top'));
  var fns = 0;
  for (var k = 0; k < 20; ++k) {
    var f = function () {};
    Object.setPrototypeOf(f, mid);
    if (getName(f) === 'f')
      ++fns;
  }
  print(fns);
  print(getName(new Proxy({}, {get: function () { return 'trap'; }})));
  print(getName(Object.create(null)));
}
main();

// OUT: 250
// OUT-NEXT: 20
// OUT-NEXT: 20
// OUT-NEXT: trap
// OUT-NEXT: undefined
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getName' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getName' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumByIdChainHits: 4{{$}}
```

`test/jit/getbyid-chain-hcid.js` (Review Focus 5), with
`-Xjit-hc-id-limit=1` added to all three RUN lines after
`-Xjit-recompile-threshold=16`:

```js
// One lazy JIT ID exists in the process. Receivers A and B read p two
// prototypes up, alternating; calls 1 and 2 record A's and B's chain
// entries, in that order, and call 16 recompiles. Version 2 gives A the
// only ID and marks B unemittable, so B is never progress: exactly one
// recompile. B's misses cross three times without change and demote the
// site, to _jit_get_by_id_refresh since it holds A's emitted entry. A's
// calls in version 2 (even i in 16..198: 92) hit inline. No function
// compiles warm before get's version 2, so nothing else takes the ID.
// Fails if a chain entry with no ID is skipped without `unemittable`
// (B stays progress: a second recompile).
function main() {
  function mk(v) {
    return Object.create(Object.create({p: v}));
  }
  function get(o) {
    return o.p;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  print(drive(get, [mk(1), mk(2)], 200));
}
main();

// OUT: 300
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainHits: 92{{$}}
```

- [ ] **Step 3: See them fail**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-chain' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: every CNT run FAILS (`NumByIdChainHits` absent) and every DUMP
run FAILS (no ` chain,` in the summary). OUT runs may pass; they pin
semantics.

- [ ] **Step 4: Counter, record flag, dump line, progress**

In `include/hermes/VM/JIT/JitCounters.h`, append after
`X(NumByIdChainRefreshes)` (moving the trailing backslash):

```cpp
  X(NumByIdChainRefreshes)                                        \
  /* Inline hits of emitted GetById chain entries, counted by the \
   * emitted code only with -Xjit-emit-counters. */               \
  X(NumByIdChainHits)
```

In `JitByIdSiteRecord` (JitFunctionData.h), after `chainEmitted`
(line 177):

```cpp
  /// The owning body emitted at least one chain entry here. Emitted-tier
  /// field: reset on carry-forward; feeds the dump line only.
  uint8_t chainEntryEmitted = 0;
```

In `lib/VM/JIT/JitCompiler.cpp`: in the carry-forward loop add
`r->chainEntryEmitted = 0;` after `r->chainEmitted = 0;` (line 150).
In `byIdSiteCanProgress`, delete the two-line chain skip added in
Task 4, restoring the body to:

```cpp
  for (const auto &e : s.entries)
    if (e.isLive() && !e.emitted && !e.unemittable)
      return true;
  return false;
```

Replace the dump block (lines 712-727) with:

```cpp
      // The ById shape records: sites holding a live shape or marked
      // megamorphic, how many of them this body gave a chain, and how
      // many of those emitted a chain entry.
      size_t recorded = 0, poly = 0, chain = 0, mega = 0;
      for (const auto &s : llvh::make_pointee_range(cr->byIdSites)) {
        bool live = false;
        for (const auto &e : s.entries)
          live |= e.isLive();
        if (live || s.megamorphic)
          ++recorded;
        poly += s.chainEmitted;
        chain += s.chainEntryEmitted;
        mega += s.megamorphic;
      }
      if (recorded) {
        llvh::outs() << "JIT ById sites: " << recorded << " recorded, " << poly
                     << " polymorphic, " << chain << " chain, " << mega
                     << " megamorphic\n";
      }
```

- [ ] **Step 5: arm64 interim**

In the arm64 `preparePolyChain`, replace the Task 4 skip with:

```cpp
      // The arm64 emitter does not emit chain entries yet: mark them, so
      // they are not progress until a compile can emit them.
      if (e.kind == JitByIdEntryKind::Chain) {
        e.unemittable = 1;
        continue;
      }
```

- [ ] **Step 6: x86-64 emission**

In `lib/VM/JIT/x86-64/JitEmitter-property.cpp`, replace `PolyEntry` and
`poly_` (:734-746) with:

```cpp
  /// One chain entry, resolved to lazy JIT IDs.
  struct PolyEntry {
    /// Lazy JIT ID of the receiver's class.
    uint16_t objId;
    /// Proto only: lazy JIT ID of the prototype's class.
    uint16_t protoId;
    /// Slot to load, in the receiver, the prototype, or the object
    /// `depth` levels up.
    SlotIndex slot;
    /// Which record entry kind this is.
    JitByIdEntryKind kind;
    /// Chain only: prototypes from the receiver to the holder.
    uint8_t depth;
    /// Chain only: the address of the record entry's epoch, which the
    /// emitted code reads. Stable: entries live in the record's inline
    /// storage and are never moved, erased or, once emitted, overwritten.
    const uint64_t *epochAddr;
  };
  /// The entries emitPolyChain() emits: own entries, then chain entries,
  /// then prototype entries.
  llvh::SmallVector<PolyEntry, JitByIdSiteRecord::kMaxEntries> poly_;

  /// \return the position of \p kind in the chain's layout.
  static unsigned emitRank(JitByIdEntryKind kind) {
    return kind == JitByIdEntryKind::Own ? 0
        : kind == JitByIdEntryKind::Chain ? 1
                                          : 2;
  }
```

Replace the whole body of `preparePolyChain` after the
`addCacheShape` call (the loop through `return true;`, :1093-1114) with:

```cpp
    Runtime &rt = _.runtime_;
    bool anyChain = false;
    for (auto &e : site_->entries) {
      if (!e.isLive())
        continue;
      bool isProto = e.kind == JitByIdEntryKind::Proto;
      bool isChain = e.kind == JitByIdEntryKind::Chain;
      // A chain entry needs a lazy ID for its receiver class only.
      uint16_t objId = _.initHCLazyIDMayAlloc(e.objClass.get(rt, rt.getHeap()));
      uint16_t protoId = 0;
      if (objId && isProto)
        protoId = _.initHCLazyIDMayAlloc(e.protoClass.get(rt, rt.getHeap()));
      if (!objId || (isProto && !protoId)) {
        e.unemittable = 1;
        continue;
      }
      e.emitted = 1;
      anyChain |= isChain;
      poly_.push_back(
          {objId,
           protoId,
           e.slot,
           e.kind,
           e.depth,
           isChain ? &e.epoch : nullptr});
    }
    if (poly_.empty())
      return false;
    // Own entries, then chain entries, then the prototype block: chain
    // entries leave the prototype block's state untouched.
    std::stable_sort(
        poly_.begin(), poly_.end(), [](const PolyEntry &a, const PolyEntry &b) {
          return emitRank(a.kind) < emitRank(b.kind);
        });
    site_->chainEmitted = 1;
    site_->chainEntryEmitted = anyChain ? 1 : 0;
    return true;
```

(The Task 4 skip is gone with the old loop.) Replace `emitPolyChain`'s
doc comment and its own-entry loop (:1117-1145, through
`if (!anyProto) return;`) with:

```cpp
  /// Emit the chain. Entry state: temp1 = object, temp2 = compressed
  /// receiver class. Own entries, then chain entries (emitChainEntry),
  /// then the prototype block. At every `next` label and at the
  /// fallthrough to the generic tier: temp1 = object, temp2 = compressed
  /// receiver class (the prototype block restores it at its join).
  /// hwRes may alias a temp; it is written only on hit paths, which jump
  /// to contLab.
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
      if (p.kind == JitByIdEntryKind::Proto)
        anyProto = true;
      if (p.kind != JitByIdEntryKind::Own)
        continue;
      asmjit::Label next = a.newLabel();
      a.cmp(temp3.r32(), asmjit::Imm(p.objId));
      a.jne(next);
      // May clobber temp1 (indirect slot); this path ends at contLab.
      emitLoadFromSlot(hwRes.gpq(), temp1, p.slot);
      shvDecode.emitFirstCase(a);
      a.jmp(contLab);
      a.bind(next);
    }
    for (const PolyEntry &p : poly_) {
      if (p.kind == JitByIdEntryKind::Chain)
        emitChainEntry(shvDecode, p);
    }
    if (!anyProto)
      return;
```

The prototype block after it is unchanged (its loop already skips
`p.kind != JitByIdEntryKind::Proto`). After `emitPolyChain`, add:

```cpp
  /// Emit one chain entry (spec: "Emission"). Entry state: temp1 =
  /// object, temp2 = compressed receiver class, temp3 = receiver
  /// lazyJITId. A miss on either guard leaves all three untouched and
  /// clobbers only temp4, xScratch and EFLAGS. Past both guards the path
  /// counts the hit, follows the live chain p.depth levels from the
  /// receiver class's objectParent, loads the slot into hwRes and ends at
  /// contLab.
  void emitChainEntry(Emit_sh_shv_decode &shvDecode, const PolyEntry &p) {
    asmjit::Label next = a.newLabel();
    a.cmp(temp3.r32(), asmjit::Imm(p.objId));
    a.jne(next);
    // xScratch is live only from here to the epoch load.
    _.loadBits64InGp(xScratch, (uint64_t)p.epochAddr, "chain entry epoch");
    a.mov(temp4, x86::qword_ptr(xScratch));
    a.cmp(
        temp4,
        x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeParentCacheEpoch));
    a.jne(next);
    // Both guards passed; this path ends at contLab. The increment uses
    // xScratch and EFLAGS, neither live here.
    _.emitIncrementCounter(JitCounter::NumByIdChainHits);
    // temp4 = the receiver's prototype, read from its class.
    const auto &clsReg =
        emit_sh_cp_decode_non_null_preserve_input(a, temp4, temp2);
    emit_load_cp(
        a, temp4, x86::ptr(clsReg, RuntimeOffsets::hiddenClassObjectParent));
    emit_sh_cp_decode_non_null(a, temp4);
    // Two dependent loads per further level: the class, then its parent.
    // No null checks: the receiver class and the epoch fix every level.
    for (unsigned i = 1; i < p.depth; ++i) {
      emit_load_cp(a, temp4, x86::ptr(temp4, offsetof(SHJSObject, clazz)));
      emit_sh_cp_decode_non_null(a, temp4);
      emit_load_cp(
          a, temp4, x86::ptr(temp4, RuntimeOffsets::hiddenClassObjectParent));
      emit_sh_cp_decode_non_null(a, temp4);
    }
    // May clobber temp4 (indirect slot).
    emitLoadFromSlot(hwRes.gpq(), temp4, p.slot);
    shvDecode.emitFirstCase(a);
    a.jmp(contLab);
    a.bind(next);
  }
```

- [ ] **Step 7: Update the existing dump lines**

In each of these lines insert `0 chain, ` before the megamorphic count,
e.g. `JIT ById sites: 1 recorded, 1 polymorphic, 0 megamorphic` becomes
`JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic`:
test/jit/getbyid-poly-mixed.js:57, getbyid-poly-mega.js:38,
getbyid-poly-own.js:39, getbyid-poly-proto.js:47, count.js:56 and :67,
recompile-phased-warming.js:48 and :69, and x86-64/exceptions.js:385 and
:393 (`{{[0-9]+}} polymorphic, 0 chain, 0 megamorphic`). None of these
sites reads a property two or more prototypes up (each file's comments
name the shapes it records); if a run shows a nonzero chain count there,
find the site before changing the expectation and explain it in the
test's comment.

- [ ] **Step 8: Run the new tests**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-chain' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all PASS. Every expected count is derived in its test's
comment; if a count differs, find the cause before touching the
expectation (a run with `-Xdump-jitcode=3` plus a temporary
`llvh::errs()` print of `site.entries` in `chainAfterForward` shows what
was recorded), and either fix the code or correct the derivation in
the comment. Then confirm once, in a `-Xdump-jitcode=3` run of
getbyid-chain-depth.js, that getD3's version-2 chain entry loads
`propStorage` before the slot (an indirect load); if not, raise `pad`
(staying below 64 properties and keeping mk()'s declines below 16) and
update the comment.

- [ ] **Step 9: Prove they can fail**

One at a time, rebuild after each change and each restore:
- Mutation A: in `emitChainEntry`, delete the epoch `cmp` and its
  `jne next` → getbyid-chain-invalidate.js FAILS (case 1 prints
  `1 1`).
- Mutation B: in `preparePolyChain`, `continue` for chain entries
  before the ID lookup (nothing emitted, nothing marked) →
  getbyid-chain.js FAILS (`0 chain`, NumByIdChainHits 0).
- Mutation C: in `jitWalkChain`, allow depth 9 (loop bound
  `kMaxChainDepth + 1`) → getbyid-chain-depth.js FAILS
  (NumByIdChainHits 336).
- Mutation D: in `chainAfterForward`'s no-room branch, `return` before
  `addByIdShape` → getbyid-chain-mega.js FAILS (`0 megamorphic`).
- Mutation E: in `isChainExcluded`, drop `clazz->isDictionary() ||` →
  getbyid-chain-exclude.js FAILS (`1 chain` for get).
- Mutation F: in `preparePolyChain`, skip a chain entry with no ID
  without setting `unemittable` → getbyid-chain-hcid.js FAILS
  (`NumRecompiles: 2`).
- Mutation G: in `emitChainEntry`, delete the `shvDecode.emitFirstCase(a)`
  call after the chain slot load (the `jmp contLab` stays) → rebuild
  cmake-build-x86jit-hv32 and cmake-build-x86jit-boxed and run
  getbyid-chain-depth.js on each: it FAILS on both (the depth-2 field of
  OUT is below 100, because 1.1 is returned undecoded). On HV64
  `emitFirstCase` emits nothing, so the HV64 tree passes; say so.

- [ ] **Step 10: Suites**

```bash
cmake --build cmake-build-x86jit --target check-hermes
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes &&
  LIT_FILTER='jit/' cmake --build cmake-build-arm64 --target check-hermes ||
  echo "FAIL arm64"
```

Expected: no failure. On x86, an existing test with a GetById site
reading a property two or more prototypes up now gets a chain entry
(an extra recompile, inline hits): reconcile as in Task 4 Step 8,
explaining every changed number. On arm64 such a site spends the same
recompile and marks the entry `unemittable`, so recompile counts match
x86; a dump line can differ only in its chain count.

- [ ] **Step 11: Commit**

```bash
git add include/hermes/VM/JIT/JitCounters.h include/hermes/VM/JIT/JitFunctionData.h \
  lib/VM/JIT/JitCompiler.cpp lib/VM/JIT/x86-64/JitEmitter-property.cpp \
  lib/VM/JIT/arm64/JitEmitter-property.cpp \
  test/jit/getbyid-chain.js test/jit/getbyid-chain-depth.js \
  test/jit/getbyid-chain-invalidate.js test/jit/getbyid-chain-mega.js \
  test/jit/getbyid-chain-exclude.js test/jit/getbyid-chain-hcid.js \
  test/jit/getbyid-poly-mixed.js test/jit/getbyid-poly-mega.js \
  test/jit/getbyid-poly-own.js test/jit/getbyid-poly-proto.js \
  test/jit/count.js test/jit/recompile-phased-warming.js \
  test/jit/x86-64/exceptions.js
git commit   # "JIT: emit GetById chain entries on x86-64"
```

(Add any other test file Step 10 changed, by path.)

---

### Task 6: arm64 chain emission

**Files:**
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp` (`PolyEntry`
  :829-841, `preparePolyChain` :1178-1208, `emitPolyChain` :1210-1269,
  new `emitChainEntry`)
- Modify: the six Task 5 tests (drop the x86-only lines)

**Interfaces:**
- Consumes: everything from Tasks 1-5; arm64 `emit_cmp_imm32`
  (arm64/JitEmitter-internal.h:572-583), `loadBits64InGp`
  (arm64/JitEmitter.h:2231-2241: a `mov` or an RO-data literal `ldr`, no
  scratch), decode helpers (arm64/JitEmitter-internal.h:498-540),
  `emitIncrementCounter` (arm64/JitEmitter.cpp:656-670, saves x0/x1
  itself).
- Produces: arm64 `PolyEntry`, `emitRank`, `emitChainEntry` with the
  x86 signatures.

Register use (spec "arm64"): `xTemp3` holds the receiver ID; the ID
compare's immediate scratch is `w16` (xScratch); after it `x16` is free
for the runtime epoch; `x17` (xScratch2, arm64/JitEmitter.h:205-209) is
used by nothing else in `GetByIdImpl` and holds the entry's epoch
address. A miss clobbers `xTemp4`, `x16`, `x17` and NZCV only.
`emitLoadFromSlot(hwRes, xTemp4, xTemp2, slot)` uses `xTemp2` as its
large-offset temporary (allowed: `tmpReg != objReg`, :976), on the hit
path only, as the prototype block does (:1261).

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: See the tests fail on arm64**

Remove, from each of getbyid-chain.js, -depth.js, -invalidate.js,
-mega.js, -exclude.js and -hcid.js, the lines
`// REQUIRES: jit, jit-arch-x86-64` and
`// x86-64 only until the arm64 emitter handles chain entries.`, and
add `// REQUIRES: jit` in place of the first. Then:

```bash
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
LIT_FILTER='getbyid-chain' cmake --build cmake-build-arm64 --target check-hermes
```

Expected: the CNT and DUMP runs of the six FAIL on arm64 (entries are
`unemittable`: `0 chain`, NumByIdChainHits 0).

- [ ] **Step 3: The arm64 emitter**

In `lib/VM/JIT/arm64/JitEmitter-property.cpp`, replace `PolyEntry` and
`poly_` (:829-841) with the x86 Task 5 block verbatim (the `PolyEntry`
with `kind`, `depth` and `epochAddr`, `poly_`, and `emitRank`):

```cpp
  /// One chain entry, resolved to lazy JIT IDs.
  struct PolyEntry {
    /// Lazy JIT ID of the receiver's class.
    uint16_t objId;
    /// Proto only: lazy JIT ID of the prototype's class.
    uint16_t protoId;
    /// Slot to load, in the receiver, the prototype, or the object
    /// `depth` levels up.
    SlotIndex slot;
    /// Which record entry kind this is.
    JitByIdEntryKind kind;
    /// Chain only: prototypes from the receiver to the holder.
    uint8_t depth;
    /// Chain only: the address of the record entry's epoch, which the
    /// emitted code reads. Stable: entries live in the record's inline
    /// storage and are never moved, erased or, once emitted, overwritten.
    const uint64_t *epochAddr;
  };
  /// The entries emitPolyChain() emits: own entries, then chain entries,
  /// then prototype entries.
  llvh::SmallVector<PolyEntry, JitByIdSiteRecord::kMaxEntries> poly_;

  /// \return the position of \p kind in the chain's layout.
  static unsigned emitRank(JitByIdEntryKind kind) {
    return kind == JitByIdEntryKind::Own ? 0
        : kind == JitByIdEntryKind::Chain ? 1
                                          : 2;
  }
```

Replace the body of `preparePolyChain` after its `addCacheShape` call
(the loop, including Task 5's interim `unemittable` branch, through
`return true;`) with:

```cpp
    Runtime &rt = _.runtime_;
    bool anyChain = false;
    for (auto &e : site_->entries) {
      if (!e.isLive())
        continue;
      bool isProto = e.kind == JitByIdEntryKind::Proto;
      bool isChain = e.kind == JitByIdEntryKind::Chain;
      // A chain entry needs a lazy ID for its receiver class only.
      uint16_t objId = _.initHCLazyIDMayAlloc(e.objClass.get(rt, rt.getHeap()));
      uint16_t protoId = 0;
      if (objId && isProto)
        protoId = _.initHCLazyIDMayAlloc(e.protoClass.get(rt, rt.getHeap()));
      if (!objId || (isProto && !protoId)) {
        e.unemittable = 1;
        continue;
      }
      e.emitted = 1;
      anyChain |= isChain;
      poly_.push_back(
          {objId,
           protoId,
           e.slot,
           e.kind,
           e.depth,
           isChain ? &e.epoch : nullptr});
    }
    if (poly_.empty())
      return false;
    // Own entries, then chain entries, then the prototype block: chain
    // entries leave the prototype block's state untouched.
    std::stable_sort(
        poly_.begin(), poly_.end(), [](const PolyEntry &a, const PolyEntry &b) {
          return emitRank(a.kind) < emitRank(b.kind);
        });
    site_->chainEmitted = 1;
    site_->chainEntryEmitted = anyChain ? 1 : 0;
    return true;
```

Replace `emitPolyChain`'s doc comment and own-entry loop (:1210-1239,
through `if (!anyProto) return;`) with:

```cpp
  /// Emit the chain. Entry state: xTemp1 = object, xTemp2 = compressed
  /// receiver class. Own entries, then chain entries (emitChainEntry),
  /// then the prototype block. At every `next` label and at the
  /// fallthrough to the generic tier: xTemp1 = object, xTemp2 =
  /// compressed receiver class (the prototype block restores it at its
  /// join). hwRes may alias a temp; it is written only on hit paths,
  /// which branch to contLab. ID compares use xScratch for IDs that are
  /// not encodable as a compare immediate.
  void emitPolyChain(Emit_sh_shv_decode &shvDecode) {
    _.comment("// Get polymorphic chain (%u entries)", (unsigned)poly_.size());
    // xTemp3 = receiver lazyJITId, decoded without clobbering xTemp2.
    // Loaded even if every entry is a prototype entry.
    const auto &xReg =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp3, xTemp2);
    a.ldrh(xTemp3.w(), a64::Mem(xReg, RuntimeOffsets::hiddenClassLazyJITId));
    bool anyProto = false;
    for (const PolyEntry &p : poly_) {
      if (p.kind == JitByIdEntryKind::Proto)
        anyProto = true;
      if (p.kind != JitByIdEntryKind::Own)
        continue;
      asmjit::Label next = a.newLabel();
      emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
      a.b_ne(next);
      // May clobber xTemp1 (indirect slot) and xTemp4; this path ends
      // at contLab.
      emitLoadFromSlot(hwRes.a64GpX(), xTemp1, xTemp4, p.slot);
      shvDecode.emitFirstCase(a);
      a.b(contLab);
      a.bind(next);
    }
    for (const PolyEntry &p : poly_) {
      if (p.kind == JitByIdEntryKind::Chain)
        emitChainEntry(shvDecode, p);
    }
    if (!anyProto)
      return;
```

After `emitPolyChain`, add:

```cpp
  /// Emit one chain entry (spec: "Emission", arm64). Entry state: xTemp1
  /// = object, xTemp2 = compressed receiver class, xTemp3 = receiver
  /// lazyJITId. A miss on either guard leaves all three untouched and
  /// clobbers only xTemp4, x16, x17 and NZCV. Past both guards the path
  /// counts the hit, follows the live chain p.depth levels from the
  /// receiver class's objectParent, loads the slot into hwRes (xTemp2 as
  /// the large-offset temporary) and ends at contLab.
  void emitChainEntry(Emit_sh_shv_decode &shvDecode, const PolyEntry &p) {
    asmjit::Label next = a.newLabel();
    emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
    a.b_ne(next);
    // x17 = &entry.epoch; x16 = the runtime's epoch (the static_assert
    // above GetByIdImpl keeps its offset LDR-encodable).
    _.loadBits64InGp(xScratch2, (uint64_t)p.epochAddr, "chain entry epoch");
    a.ldr(xTemp4, a64::Mem(xScratch2));
    a.ldr(
        xScratch,
        a64::Mem(xRuntime, RuntimeOffsets::runtimeParentCacheEpoch));
    a.cmp(xTemp4, xScratch);
    a.b_ne(next);
    // Both guards passed; this path ends at contLab.
    _.emitIncrementCounter(JitCounter::NumByIdChainHits);
    // xTemp4 = the receiver's prototype, read from its class.
    const auto &xCls =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp4, xTemp2);
    emit_load_cp(
        a, xTemp4, a64::Mem(xCls, RuntimeOffsets::hiddenClassObjectParent));
    emit_sh_cp_decode_non_null(a, xTemp4);
    // Two dependent loads per further level: the class, then its parent.
    // No null checks: the receiver class and the epoch fix every level.
    for (unsigned i = 1; i < p.depth; ++i) {
      emit_load_cp(a, xTemp4, a64::Mem(xTemp4, offsetof(SHJSObject, clazz)));
      emit_sh_cp_decode_non_null(a, xTemp4);
      emit_load_cp(
          a, xTemp4, a64::Mem(xTemp4, RuntimeOffsets::hiddenClassObjectParent));
      emit_sh_cp_decode_non_null(a, xTemp4);
    }
    // Clobbers xTemp4 (indirect slot) and xTemp2 (large offset).
    emitLoadFromSlot(hwRes.a64GpX(), xTemp4, xTemp2, p.slot);
    shvDecode.emitFirstCase(a);
    a.b(contLab);
    a.bind(next);
  }
```

The prototype block after `if (!anyProto) return;` is unchanged.

- [ ] **Step 4: Build all three arm64 trees and run**

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  ./aarch64/qemu-sanity.sh $t &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no `FAIL`; the six chain tests and getbyid-chain-record.js
pass on every arm64 tree with the same counts as on x86 (the tests pin
only comments, counters, summary lines and output, which are backend
neutral). An arm64-only failure is a finding about the port: debug it
(superpowers:systematic-debugging) and fix the arm64 emitter; never
gate the test.

- [ ] **Step 5: Prove the arm64 emission is pinned**

On cmake-build-arm64 (rebuild host first; restore and rebuild after
each):
- Mutation A: delete the epoch `cmp`/`b_ne next` pair in arm64
  `emitChainEntry` → getbyid-chain-invalidate.js FAILS (`1 1`).
- Mutation B: in arm64 `emitChainEntry`, start the level loop at
  `i = 2` → getbyid-chain-depth.js FAILS (wrong values for depths 3
  and 8).

- [ ] **Step 6: x86 unchanged**

```bash
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: the same pass count as after Task 5 (`git diff --stat HEAD`
shows only the arm64 emitter and the six test files).

- [ ] **Step 7: Commit**

```bash
git add lib/VM/JIT/arm64/JitEmitter-property.cpp test/jit/getbyid-chain.js \
  test/jit/getbyid-chain-depth.js test/jit/getbyid-chain-invalidate.js \
  test/jit/getbyid-chain-mega.js test/jit/getbyid-chain-exclude.js \
  test/jit/getbyid-chain-hcid.js
git commit   # "JIT: emit GetById chain entries on arm64"
```

---

### Task 7: Refresh, demotion, retirement and rooting tests

Tests of Task 4's refresh paths through emitted entries, on both
backends; a fix goes in this task if one exposes a bug (debug with
superpowers:systematic-debugging; the test becomes the regression
gate).

**Files:**
- Test: `test/jit/getbyid-chain-refresh.js`,
  `getbyid-chain-refresh-demoted.js`,
  `getbyid-chain-refresh-revalidate.js`,
  `getbyid-chain-refresh-mapless.js`, `getbyid-chain-refresh-settle.js`,
  `getbyid-chain-retire.js`, `getbyid-chain-rooted.js`

**Interfaces:**
- Consumes: everything from Tasks 1-6.
- Produces: nothing new.

**Spec resolution (test files).** The spec's single
getbyid-chain-refresh.js with variants (a)-(d) is split into five files,
because the counters are process-wide and each variant's exact counts
must be independent. **Spec resolution (refresh base case).** A single
unrelated bump cannot distinguish the fast refresh from the
revalidation after forwarding (both refresh once, and the hits are the
same); 48 bump-and-read rounds do, because only the fast refresh avoids
the 48 declines that would demote the site. **Spec resolution
(variant c).** As written in the spec, the forwarded lookups of version
1 (calls 2-16) call `findProperty` on the holder's mapless flagged
class, which steals C's map (`initializeMissingPropertyMap`,
HiddenClass.cpp:1126-1136), and every later forwarded lookup rebuilds
it, so the fast refresh would see a map and never reach
`findPropertyNoAlloc`. Here 48 flagged twins share the holder's
flagged class C'; before each read one twin adds a NEW property, which
first rebuilds C''s map (its lookup) and then moves it to the new child
(HiddenClass.cpp:553-568), so C' is mapless at every fast refresh while
C stayed mapless since the steal. The pre-fix failure is then visible as
48 declines (a demotion), not as a refresh count: the revalidation after
the forwarded lookup finds the right slot through the rebuilt map.

Every file: copyright header; the RUN lines named; `// REQUIRES: jit`;
`// UNSUPPORTED: handle_san` unless stated otherwise.

- [ ] **Step 1: Write the tests**

`test/jit/getbyid-chain-refresh.js` — OUT and CNT RUN lines (the first
and third standard RUN lines of Task 5):

```js
// The fast refresh. drive(get, recv, 20): calls 1-16 decline in version
// 1 (call 1 records the chain entry and flags mid and base), call 16
// recompiles, calls 17-20 hit (4). Then 48 rounds of: bump(unrel, i)
// adds a new property to unrel, flagged by the cached add `kid.k = 1`,
// which bumps the epoch; the read misses on the stale epoch, and the
// helper's fast refresh walks the flagged path to the same (depth,
// slot), refreshes the entry and returns the value without forwarding
// or counting (48 refreshes, no decline). drive(get, recv, 10): 10 hits.
// NumByIdChainHits = 4 + 10 = 14.
// Fails without the fast refresh: the revalidation after forwarding
// would refresh just as often, but 48 counted declines would demote the
// site (NumByIdDemotions: 1). bump()'s keyed store is a ByVal site; its
// own declines do not affect NumByIdDemotions.
function main() {
  var base = {p: 5};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var unrel = {};
  var kid = Object.create(unrel);
  kid.k = 1;
  function get(o) {
    return o.p;
  }
  function bump(o, i) {
    o['y' + i] = i;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function churn(f, o, b, u, n) {
    var s = 0;
    for (var i = 0; i < n; ++i) {
      b(u, i);
      s += f(o);
    }
    return s;
  }
  var s1 = drive(get, recv, 20);
  var s2 = churn(get, recv, bump, unrel, 48);
  var s3 = drive(get, recv, 10);
  print(s1, s2, s3);
}
main();

// OUT: 100 240 50
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 48{{$}}
// CNT: NumByIdChainHits: 14{{$}}
```

`test/jit/getbyid-chain-refresh-demoted.js` (variant a) — OUT and CNT:

```js
// The refresh after demotion. drive(get, recv, 20): 4 hits as in
// getbyid-chain-refresh.js. miss(get, none, 48): `none` has no p; each
// call misses the chain and declines in version 2 without changing the
// record (the walk is Rejected for a class with no entries), so the
// three crossings are stable and the third demotes the site. It holds
// an emitted chain entry, so the slot becomes _jit_get_by_id_refresh.
// `unrel.y = 2` bumps the epoch. drive(get, recv, 10): the first call
// misses, the refresh helper's fast refresh refreshes (1) and returns
// the value; 9 hits. NumByIdChainHits = 13.
// Fails if demotion always flips to the plain helper (no refresh, 4
// hits).
function main() {
  var base = {p: 5};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var none = {};
  var unrel = {};
  var kid = Object.create(unrel);
  kid.k = 1;
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function miss(f, o, n) {
    for (var i = 0; i < n; ++i)
      f(o);
  }
  var s1 = drive(get, recv, 20);
  miss(get, none, 48);
  unrel.y = 2;
  var s2 = drive(get, recv, 10);
  print(s1, s2);
}
main();

// OUT: 100 50
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 13{{$}}
```

`test/jit/getbyid-chain-refresh-revalidate.js` (variant b) — OUT and CNT:

```js
// Revalidation. drive(get, recv, 20): 4 hits. setPrototypeOf(mid, ...)
// bumps the epoch (mid is flagged) and puts a new, unflagged object
// with p at the same slot (0) where base was. The next call misses; the
// fast refresh walks a path with an unflagged object and does nothing;
// after forwarding, revalidation flags the new object and adopts the
// epoch (refresh 1, one decline). 9 hits follow. NumByIdChainHits = 13.
// Fails without revalidation (0 refreshes, 4 hits).
function main() {
  var base = {p: 5, w: 0};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  var s1 = drive(get, recv, 20);
  Object.setPrototypeOf(mid, {p: 7, z: 0});
  var s2 = drive(get, recv, 10);
  print(s1, s2);
}
main();

// OUT: 100 70
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 13{{$}}
```

`test/jit/getbyid-chain-refresh-mapless.js` (variant c) — OUT and CNT:

```js
// The fast refresh through a holder class with no property map, whose p
// went through a flags transition (adee03e65, dz 01a170e9).
// mkHolder(): {p: 5, q: 6}, then p made non-enumerable: a flags
// transition into class C that keeps p at slot 0; C takes the map.
// holder and 48 twins are built this way, all on C. flag(t) does a
// cached add on an object whose parent is t, which flags t
// (JSObject::tryCacheAddProperty): the first creates C', C's flagged
// child, without a map (HiddenClass.cpp:790-797); every twin moves to
// C'. holder is still on C.
// drive(get, recv, 20): call 1 walks C's map, finds p at depth 2, slot
// 0, and flags mid and holder (holder moves to C'). Calls 2-16 forward;
// their lookups steal C's map into C' (HiddenClass.cpp:1132-1136), so C
// is mapless from here on. Call 16 recompiles; calls 17-20 hit (4).
// 48 rounds of: bumpTwin(twins[i], i) adds a NEW property to a twin on
// C', which rebuilds C''s map and moves it to the new child, and bumps
// the epoch (the twin is flagged); then the read's fast refresh walks
// mid, then holder's mapless C', so findPropertyNoAlloc walks C' -> C
// (p's flags transition) -> ... and must return slot 0. It refreshes
// and returns without forwarding (48 refreshes, no decline). Then 10
// hits. NumByIdChainHits = 14.
// With adee03e65's HiddenClass.cpp hunk reverted, findPropertyNoAlloc
// returns slot 1 at C, the fast refresh never matches, every round
// forwards (rebuilding C''s map, so revalidation still refreshes) and
// declines, and 48 declines demote the site: NumByIdDemotions: 1.
// Setup declines (none of these functions reads p): mkHolder 1 in
// total (its Object.defineProperty read misses once, then hits the
// generic cache), flag 49 (one Object.create miss plus 48 PutById
// misses, JitHandlers.cpp:358), bumpTwin 48.
function main() {
  function mkHolder() {
    var h = {p: 5, q: 6};
    Object.defineProperty(h, 'p', {enumerable: false});
    return h;
  }
  function flag(t) {
    var k = Object.create(t);
    k.k = 1;
  }
  function bumpTwin(t, i) {
    t['r' + i] = 1;
  }
  var holder = mkHolder();
  var twins = [];
  for (var i = 0; i < 48; ++i) {
    var t = mkHolder();
    flag(t);
    twins[i] = t;
  }
  var mid = Object.create(holder);
  var recv = Object.create(mid);
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function churn(f, o, b, tw, n) {
    var s = 0;
    for (var i = 0; i < n; ++i) {
      b(tw[i], i);
      s += f(o);
    }
    return s;
  }
  var s1 = drive(get, recv, 20);
  var s2 = churn(get, recv, bumpTwin, twins, 48);
  var s3 = drive(get, recv, 10);
  print(s1, s2, s3);
}
main();

// OUT: 100 240 50
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 48{{$}}
// CNT: NumByIdChainHits: 14{{$}}
```

(NumRecompiles is not pinned here: `flag`'s PutById site declines 48
times and may recompile `flag` itself.)

`test/jit/getbyid-chain-refresh-settle.js` (variant d) — OUT and CNT:

```js
// A verdict on an unflagged path suppresses nothing. drive: 4 hits.
// miss(get, none, 48) demotes the site to _jit_get_by_id_refresh (as in
// getbyid-chain-refresh-demoted.js). setPrototypeOf(mid, empty) bumps
// the epoch; `empty` is unflagged and has no p. get(recv): the refresh
// helper's fast refresh finds no p (Rejected) and does nothing; after
// forwarding (undefined), revalidation walks again, Rejected on a path
// with an unflagged object, so refreshFailedAt is NOT set. `empty.p = 9`
// changes the answer without a bump (empty is unflagged; the cached add
// flags only empty's parents). drive(get, recv, 10): the first call
// misses, the fast refresh sees an unflagged path, and the revalidation
// after forwarding (a candidate still exists) flags empty and adopts
// the epoch (refresh 1); 9 hits. NumByIdChainHits = 13.
// Fails if a Rejected verdict sets refreshFailedAt regardless of
// allFlagged (no candidate, no revalidation: 0 refreshes, 4 hits).
function main() {
  var base = {p: 5};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  var none = {};
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(o);
    return s;
  }
  function miss(f, o, n) {
    for (var i = 0; i < n; ++i)
      f(o);
  }
  var s1 = drive(get, recv, 20);
  miss(get, none, 48);
  var empty = {};
  Object.setPrototypeOf(mid, empty);
  var gone = get(recv);
  empty.p = 9;
  var s2 = drive(get, recv, 10);
  print(s1, gone, s2);
}
main();

// OUT: 100 undefined 90
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 13{{$}}
```

`test/jit/getbyid-chain-retire.js` — the three standard RUN lines:

```js
// A retired body still on the stack. rec() reads o.m once before and
// once after recursing (one site, executed twice per frame).
// Phase 1: 8 calls rec(o, 0, noop), 16 declines in version 1 (call 1
// records the chain entry); the 16th recompiles: version 2 emits it.
// Phase 2: rec(o, 4, hook) runs version 2 in frames d = 4..0, each
// reading before recursing: 5 hits. At d = 0, hook(0) drives rec with
// three own-property shapes: each call's first read declines and
// records an own entry, its second hits the generic tier; the 16th
// crosses, the own entries are progress, and version 3 is installed,
// retiring version 2 (its slot goes to the plain helper; its entries
// keep their epochs). Unwinding, frames d = 0 and 1 read after
// recursing: the retired body's chain entry still hits (2 hits). hook(2)
// adds m to mid, a flagged object: a bump and a shadowing property.
// Frames d = 2, 3, 4 then miss and read 100 through the plain helper.
// rec(o, 4, hook) = 1+(1+(1+(1+(1+1)+1)+100)+100)+100 = 307. The final
// rec(o, 0, noop) runs version 3, whose copied entry is stale: 200.
// NumByIdChainHits = 5 + 2 = 7; NumRecompiles = 2.
// Fails if retired chain checks miss (5 hits), and if the epoch compare
// is missing (frames d = 2..4 read 1, not 100).
function main() {
  var base = {m: 1};
  var mid = Object.create(base);
  var o = Object.create(mid);
  var own = [{m: 1000}, {a: 0, m: 1000}, {b: 0, m: 1000}];
  function noop(d) {}
  function rec(o, d, hook) {
    var s = 0;
    for (var k = 0; k < 2; ++k) {
      s += o.m;
      if (k === 0) {
        if (d > 0)
          s += rec(o, d - 1, hook);
        hook(d);
      }
    }
    return s;
  }
  function hook(d) {
    if (d === 0) {
      for (var i = 0; i < 16; ++i)
        rec(own[i % 3], 0, noop);
    } else if (d === 2) {
      mid.m = 100;
    }
  }
  for (var i = 0; i < 8; ++i)
    rec(o, 0, noop);
  var r = rec(o, 4, hook);
  print(r, rec(o, 0, noop));
}
main();

// OUT: 307 200
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 3)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'rec' (version 3)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdChainHits: 7{{$}}
```

`test/jit/getbyid-chain-rooted.js` (Review Focus 3) — runs on the
handle-sanitizer tree, so it has NO `UNSUPPORTED: handle_san` line:

```js
// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Pointer results across flagging, which allocates. Recording (call 1)
// flags mid and base, allocating their flagged classes, while the
// helper holds {n: 7}. setPrototypeOf(mid, ...) puts {p: {n: 70}, z: 0}
// in base's place: same depth and slot, a class ({p, z}) whose flagged
// child does not exist yet, so the revalidating call allocates it while
// holding {n: 70}. With -gc-sanitize-handles=1 a HERMESVM_SANITIZE_HANDLES
// build moves every live object at every allocation (other builds
// ignore the flag), so an unrooted result reads freed memory.
function main() {
  var base = {p: {n: 7}, y: 0};
  var mid = Object.create(base);
  var recv = Object.create(mid);
  function get(o) {
    return o.p;
  }
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  var a = drive(get, recv, 20);
  Object.setPrototypeOf(mid, {p: {n: 70}, z: 0});
  var b = drive(get, recv, 10);
  print(a.n, b.n);
}
main();

// CHECK: 7 70
```

- [ ] **Step 2: Run them on x86**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-chain' cmake --build cmake-build-x86jit --target check-hermes
cmake --build cmake-build-x86jit-hsan --target hermes
LIT_FILTER='getbyid-chain-rooted' cmake --build cmake-build-x86jit-hsan --target check-hermes
```

Expected: all PASS; on the hsan tree getbyid-chain-rooted.js RUNS (it
must not be reported unsupported). A count that differs from its
derivation is investigated before any expectation changes, as in Task
5 Step 8. For getbyid-chain-refresh.js and -mapless.js, confirm once
that the bumps happen: if NumByIdChainRefreshes is 0, check whether
`unrel` (respectively each twin) was flagged, with a temporary
`llvh::errs() << runtime.getFullParentCacheEpoch()` print in
`chainFastRefresh`; remove it afterwards.

- [ ] **Step 3: Prove they can fail**

One at a time on cmake-build-x86jit (rebuild after each change and
restore):
- Mutation A: in `_jit_get_by_id`, delete the step-1 fast refresh block
  → getbyid-chain-refresh.js FAILS (`NumByIdDemotions: 1`).
- Mutation B: in the demotion branch of `considerRecompile`, always use
  `_sh_ljs_get_by_id_rjs` → getbyid-chain-refresh-demoted.js FAILS
  (`NumByIdChainRefreshes: 0`).
- Mutation C: in `chainAfterForward`, delete `same->epoch = now;` →
  getbyid-chain-refresh-revalidate.js FAILS.
- Mutation D: `git show adee03e65 -- lib/VM/HiddenClass.cpp | git apply -R`
  → getbyid-chain-refresh-mapless.js FAILS (`NumByIdDemotions: 1`).
  Restore with `git checkout lib/VM/HiddenClass.cpp`.
- Mutation E: in `chainAfterForward`, set `refreshFailedAt` for stale
  candidates whether or not `w.allFlagged` holds →
  getbyid-chain-refresh-settle.js FAILS.
- Mutation F: in `demoteAllSitesOnRetirement`, also set `e.epoch = 0`
  for every entry → getbyid-chain-retire.js FAILS
  (`NumByIdChainHits: 5`).
On cmake-build-x86jit-hsan:
- Mutation G: in `_jit_get_by_id`, return `HermesValue::fromRaw(res.raw)`
  instead of `lv.result.getHermesValue()` → getbyid-chain-rooted.js
  FAILS (an ASan report or a wrong value). If it passes, the test is
  not collecting during flagging: verify with a temporary print in
  `flagChainPath` that `setCachedUsingEpoch` creates a class there, and
  fix the test before continuing.

- [ ] **Step 4: All trees**

```bash
cmake --build cmake-build-x86jit --target check-hermes
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no failure, no `FAIL` line; every getbyid-chain test passes
on all six trees.

- [ ] **Step 5: Commit**

```bash
git add test/jit/getbyid-chain-refresh.js test/jit/getbyid-chain-refresh-demoted.js \
  test/jit/getbyid-chain-refresh-revalidate.js \
  test/jit/getbyid-chain-refresh-mapless.js \
  test/jit/getbyid-chain-refresh-settle.js test/jit/getbyid-chain-retire.js \
  test/jit/getbyid-chain-rooted.js
git commit   # "JIT: test GetById chain refresh, demotion and retirement"
```

(Add, by path, any code file a fix in this task touched.)

---

### Task 8: doc/JIT.md

**Files:**
- Modify: `doc/JIT.md:350-397` ("Inline caches", poly chain),
  `:1131-1228` ("GetById shape records"), `:1307-1308` ("Debugging
  aids")

**Interfaces:**
- Consumes: the finished feature; record sizes measured in Step 1.

- [ ] **Step 1: Measure the record sizes**

Write `$S/chain/probe.cpp`:

```cpp
#include "hermes/VM/JIT/JitFunctionData.h"
#include "llvh/Support/raw_ostream.h"
int main() {
  hermes::vm::JitByIdSiteRecord r;
  llvh::errs() << "sizeof(JitByIdShapeEntry)="
               << sizeof(hermes::vm::JitByIdShapeEntry)
               << " sizeof(JitByIdSiteRecord)="
               << sizeof(hermes::vm::JitByIdSiteRecord) << " helperOfs="
               << ((const char *)&r.helper - (const char *)&r) << "\n";
}
```

Build it with each tree's own flags (the compile command of
`lib/VM/JIT/JitCompiler.cpp` in `<tree>/compile_commands.json`, with
`-c`, `-o <file>`, `-MD`/`-MF`/`-MT` and the source file removed, the
probe added, `-o $S/chain/probe-<tree>`, and the tree's
`libLLVHSupport.a`, `-lpthread -ldl` appended; the session scratchpad's
earlier `probe.py` does exactly this if it is still there), for
`cmake-build-x86jit` and `cmake-build-x86jit-hv32`, run both, and keep
the numbers for this task and Task 9. Cross-check `helperOfs` with the
`call qword ptr [r9+N]` of a GetById site in each tree's
`-Xdump-jitcode=3` output. Expected (by layout): entry 40 B on HV64 and
32 B on HV32; the record grows by 4 x 16 B.

- [ ] **Step 2: "Inline caches"**

In the "Entries" bullet (doc/JIT.md:361), after the first sentence, add:

> A chain entry (doc/superpowers/specs/2026-10-24-jit-getbyid-chain-design.md)
> is a receiver class, a depth (2 to `kMaxChainDepth`, 8) and a slot in
> the object that many prototypes up, valid while the runtime's 64-bit
> parent cache epoch equals the entry's `epoch`; it needs a lazy ID for
> the receiver class only.

In the "Registers" bullet, add:

> A chain entry also uses `r11` on x86-64 (the entry's epoch address,
> dead after the load) and `x16`/`x17` on arm64 (the runtime's epoch and
> the entry's epoch address).

In the "Layout" bullet, after the own-entry sentence, add:

> Chain entries come next, before the prototype block, so they leave its
> state untouched. Each is `cmp temp3, ID; jne next`, then the entry's
> epoch is loaded through its address (`mov r11, imm64; mov temp4,
> [r11]`; arm64 `ldr xTemp4, [x17]`) and compared with
> `Runtime::parentCacheEpoch_` (`cmp temp4, [r15 + ofs]`; arm64 `ldr x16,
> [x19, #ofs]; cmp`), `jne next`. Past both guards the hit is counted
> (`NumByIdChainHits`, only with `-Xjit-emit-counters`), `temp4` follows
> the receiver class's `objectParent` and then each further level's
> `clazz` and `objectParent` (two dependent loads per level), and the
> slot is loaded from it. A miss leaves `temp1`-`temp3` untouched.
> Emitted code embeds the entry's epoch address: entries live in the
> record's inline storage and are never moved, erased or, once emitted,
> overwritten.

Change the dump sentence (line 397) to: "The dump comments it `// Get
polymorphic chain (N entries)`; N counts chain entries too."

- [ ] **Step 3: "GetById shape records"**

In the "Records" bullet (line 1139), replace "(receiver class, prototype
class for a prototype entry, slot, and the `isProto`, `emitted` and
`unemittable` bytes)" with "(receiver class; prototype class for a
prototype entry; `epoch` and `refreshFailedAt` for a chain entry; slot;
`depth`; the explicit `kind`, `Own`, `Proto` or `Chain`, set when
recorded; and the `emitted` and `unemittable` bytes)", add
`chainEntryEmitted` to the list of per-site bytes, and replace the
sizes with the Step 1 numbers (and the HV64 helper offset with the
measured one, in the "Trigger" bullet too).

After the "Carry-forward and retirement" bullet add:

> - **Chain entries.** `jitWalkChain` (JitHandlers.cpp) walks from the
>   receiver's prototype up to 8 levels with class lookups only (the
>   property map, else `HiddenClass::findPropertyNoAlloc`), never
>   allocating; it rejects dictionaries, proxies, host and lazy objects
>   anywhere on the path or as the receiver, accessors, slots above
>   0xffff, and properties found on the receiver or its prototype, and
>   reports whether every object it visited is flagged
>   (`isCachedUsingEpoch`) or typed. The recording helper walks only
>   when the read cache yielded no own or prototype shape; on a new
>   (depth, slot) with room it flags the path
>   (`JSObject::setCachedUsingEpoch`, which can allocate but never
>   throws) and records the entry at the current epoch; with no room it
>   marks the site megamorphic. Before forwarding, a fast refresh under
>   `NoAllocScope` refreshes a stale entry whose (depth, slot) a fully
>   flagged walk confirms, and for an emitted entry returns the holder's
>   value without forwarding or counting a decline; an unemitted entry
>   is refreshed but its call still forwards and counts. After
>   forwarding, revalidation flags a path that now holds unflagged
>   objects and adopts the epoch if nothing bumped it. A different
>   verdict on a fully flagged path sets `refreshFailedAt`, which
>   suppresses further refresh attempts until the epoch moves.
>   `NumByIdChainRefreshes` counts the refreshes. A site that demotes
>   while holding an emitted chain entry flips to
>   `_jit_get_by_id_refresh` (fast refresh, forward, revalidation;
>   records and counts nothing), which is not a recording helper.
>   Retirement keeps a retired record's entries, because the retired body
>   may still run and read their epochs, and nulls their weak roots
>   instead, so retired records still need no tracing (Debug builds
>   assert that their entries hold no class); it flips both recording and
>   refresh slots to the plain helper. Carry-forward copies chain entries
>   whole and resets `refreshFailedAt`.
> - **The epoch.** `Runtime::parentCacheEpoch_` is 64-bit and monotonic.
>   `getParentCacheEpoch()` returns its low 24 bits for the add cache,
>   which sees the same sequence as before (the wrap still invalidates
>   every add cache entry); chain entries and the array fast path compare
>   the full counter (`getFullParentCacheEpoch()`,
>   `RuntimeOffsets::runtimeParentCacheEpoch`).

In the "Demotion" bullet, add: "A site holding an emitted chain entry
flips to `_jit_get_by_id_refresh` instead." Replace the "Dump" bullet's
format with `JIT ById sites: R recorded, P polymorphic, C chain, M
megamorphic` and add: "C counts sites where this body emitted at least
one chain entry." Append to the tests paragraph: "`getbyid-chain*.js`
cover the DeltaBlue shape, depths 1-9, invalidation by each kind of
path mutation, the fast refresh, refresh after demotion, revalidation,
a mapless flags-transition holder, unflagged verdicts, overflow,
exclusions, ID exhaustion, a retired body on the stack and rooted
results; `unittests/VMRuntime/JitChainWalkTest.cpp` covers the walk's
slot limit, its `findPropertyNoAlloc` fallback and its verdicts."

- [ ] **Step 4: "Debugging aids"**

Replace the counters bullet (lines 1307-1308) with:

> - Counters (`JIT_COUNTERS` in include/hermes/VM/JIT/JitCounters.h:
>   NumCall, NumCallSlow, NumRecompileChecks, NumRecompiles,
>   NumByValDemotions, NumByIdDemotions, NumByIdChainRefreshes,
>   NumByIdChainHits), with `-Xjit-emit-counters`. Emitted code
>   increments NumCall, NumCallSlow and NumByIdChainHits inline; the
>   runtime increments the rest (`JITContext::incrementCounter`).

- [ ] **Step 5: Commit**

```bash
git add doc/JIT.md
git commit   # "JIT: document GetById chain entries"
```

---

### Task 9: Benchmarks, delivered section, issue

**Files:**
- Modify: `doc/superpowers/specs/2026-10-24-jit-getbyid-chain-design.md`
  (append "Delivered (date)")
- Close: dz `01a13df8-970b`

**Interfaces:**
- Consumes: BASE (Task 1's report), every task's report, Task 8's sizes.

- [ ] **Step 1: Base trees**

```bash
git worktree add /home/tmikov/work/hermes-x86-jit-base <BASE>
grep -E '^(CMAKE_BUILD_TYPE|HERMESVM_ALLOW_JIT|HERMESVM_HEAP_HV_MODE|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|HERMES_ENABLE_DEBUGGER|CMAKE_CXX_FLAGS|CMAKE_C_FLAGS):' \
  cmake-build-jit2/CMakeCache.txt cmake-build-release/CMakeCache.txt
```

Configure `cmake-build-jit2` and `cmake-build-release` in the base
worktree with exactly those values (`cmake -S
/home/tmikov/work/hermes-x86-jit-base -B
/home/tmikov/work/hermes-x86-jit-base/<tree> -G Ninja -D...`), then
build `hermes hermesc` in all four trees (two head, two base). If the
worktree path already exists from earlier work, check it out at BASE
instead of adding it.

- [ ] **Step 2: Compile and run**

For each build, compile every Octane benchmark (richards deltablue
crypto raytrace earley-boyer regexp splay navier-stokes pdfjs mandreel
gbemu box2d zlib typescript) from `benchmarks/octane/<b>.js` with that
build's own `hermesc -O -emit-binary -out $S/chain/hbc/<build>/<b>.hbc`.
Run with nothing else building or testing, following the earlier
script (session scratchpad `run_bench.sh`): 5 reps; per rep, per
benchmark, base and head interleaved, order alternating with rep
parity; `taskset -c 2 <tree>/bin/hermes -Xjit <hbc>` for the JIT trees
(`cmake-build-jit2`) and `taskset -c 2 <tree>/bin/hermes -Xjit=off
<hbc>` for the interpreter trees (`cmake-build-release`); write
`mode,rep,benchmark,build,score,exit` rows to `$S/chain/scores.csv`
and keep every log. Summarize per mode and benchmark: median
[min-max] for base and head, head/base, verdict "noise" if the ranges
overlap, else faster/slower; plus the geomean.

Expected (spec "Benchmarks"): DeltaBlue's JIT score rises well beyond
noise; no JIT benchmark regresses beyond noise; interpreter scores
unchanged within noise. Results that miss expectations are reported as
measured, with a profile, not tuned away.

- [ ] **Step 3: Slow GetById calls in DeltaBlue**

Apply the temporary instrumentation (never committed) to the head
worktree: `git apply <session scratchpad>/miss-instrumentation.diff`. If
its JitHandlers.cpp hunk no longer applies, apply the StaticH.cpp part
alone and add by hand, in lib/VM/JIT/JitHandlers.cpp, the declaration
`extern "C" void hermesMissSetViaRecorder(bool v);` and
`hermesMissSetViaRecorder(true);` / `hermesMissSetViaRecorder(false);`
around the forwarded `_sh_ljs_get_by_id_rjs` call in both
`_jit_get_by_id` and `_jit_get_by_id_refresh`. Rebuild head
`cmake-build-jit2`, run
`HERMES_MISS_DUMP=$S/chain/miss-head.tsv taskset -c 2
cmake-build-jit2/bin/hermes -Xjit $S/chain/hbc/head-jit2/deltablue.hbc`,
and do the same in the base worktree (apply, rebuild, run to
`miss-base.tsv`). Record each file's `TOTAL` line. Expected: head's
total is below 10% of base's (dz 970b measured 31.8M at e5c158faa).
Then remove the instrumentation from both trees
(`git checkout lib/VM/StaticH.cpp lib/VM/JIT/JitHandlers.cpp` in each)
and rebuild both `cmake-build-jit2` trees; `git status` must show no
modified tracked file.

- [ ] **Step 4: Delivered section**

Append to the spec, in the style of the poly spec's "Delivered
(2026-10-14)" section:

```markdown
## Delivered (YYYY-MM-DD)

Commits `<first>..<last>` on branch `x86-jit` (base `<BASE>`).

### Differences from the text above
(one bullet per spec resolution in this plan's last section, plus
anything the implementation changed, each with its reason)

### Matrix
(table: tree | configuration | result, from the task reports: full
check-hermes on cmake-build-x86jit, -hv32 and -boxed; the JIT suite and
qemu-sanity on the three arm64 trees; getbyid-chain-rooted.js on
cmake-build-x86jit-hsan; the mutations of Tasks 3-7 and what each
failed)

### Sizes
(table: mode | JitByIdShapeEntry | JitByIdSiteRecord | helper offset,
HV64 and HV32, before (24/136/128 and 16/104/96) and after)

### Benchmarks
(the exact commands, then two tables, JIT and interpreter:
benchmark | base median [min-max] | head median [min-max] | head/base |
verdict, with the geomean; then DeltaBlue's slow GetById totals, base
and head, and their ratio)
```

Every value comes from a run made in this task or a task report;
nothing is estimated.

- [ ] **Step 5: Close the issue**

If DeltaBlue met the spec's expectations:

```bash
dz close 01a13df8-970b --as fixed -m - --json <<'EOF'
GetById chain entries (doc/superpowers/specs/2026-10-24-jit-getbyid-chain-design.md,
"Delivered") inline data properties 2-8 prototypes up on x86-64 and
arm64. DeltaBlue slow GetById calls: <base TOTAL> -> <head TOTAL>; JIT
score <base median> -> <head median>.
EOF
```

with the measured numbers filled in. Otherwise
`dz comment 01a13df8-970b -m -` with the same measurements and what
remains, and leave it open.

- [ ] **Step 6: Commit**

```bash
git add doc/superpowers/specs/2026-10-24-jit-getbyid-chain-design.md \
  dz/issues/01a13df8-970b-7304-871c-763ccd425c3c.md
git commit   # "JIT: record the delivered GetById chain measurements"
```

---

## Spec resolutions (summary)

1. Fast refresh of an unemitted entry: refreshed, but the call forwards
   and counts (Task 4); only emitted entries complete the call early.
   The literal text would starve the recompile trigger in programs that
   bump the epoch between misses.
2. "Harvested" means the own or prototype branch of
   `recordByIdObservation` offered a shape to `addByIdShape` (Task 4).
3. The dump's chain count uses a new per-site byte,
   `JitByIdSiteRecord::chainEntryEmitted` (Task 5).
4. `NumByIdChainRefreshes` is counted in C++ through a new
   `JITContext::incrementCounter` (Task 4).
5. Interim states: Task 4 records but emits nothing (chain entries are
   not progress); Task 5 emits on x86-64 while arm64 marks chain
   entries `unemittable` and the new tests are x86-only; Task 6 lifts
   both.
6. The refresh test is five files, and its base case and variant (c)
   are redesigned so that their claims are observable (Task 7):
   repeated bump-and-read rounds make the fast refresh distinguishable
   from revalidation (by the demotion it avoids), and 48 flagged twins
   keep the holder's flagged class mapless at every fast refresh, which
   the spec's sequence did not (version 1's forwarded lookups steal the
   map).
7. The invalidate test's `setPrototypeOf` case moves `p` to a new slot;
   the same-slot replacement is the revalidation test (Task 5).
8. The exclude test adds an own-property receiver so that the excluded
   site compiles a version 2 whose dump can show `0 chain`, and puts
   `name` two prototypes up from functions with `setPrototypeOf`, since
   `Function.prototype` has its own `name` (Task 5).
9. If the arm64 offset of `parentCacheEpoch_` is not LDR-encodable, the
   Task 1 static_assert stops the plan for a decision.

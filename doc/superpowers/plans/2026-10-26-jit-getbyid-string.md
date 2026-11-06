# JIT GetById on String Receivers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serve GetById on string primitive receivers inline in JIT code,
on x86-64 and arm64 in every heap mode: `length` from the string header,
and data properties of `String.prototype` itself through a class-ID
guarded slot load, so that Octane TypeScript's string-receiver slow calls
(22.2M, dz `01a16b01`) drop by more than 90%.

**Architecture:** The site record learns one bit, `sawString`, from the
recording helper. A compile that finds the bit prepares the site
(`prepareByIdString`, shared by both backends): `length` becomes a Length
tier, a data property of `String.prototype`'s class a Method tier guarded
by that class's lazy JIT ID, anything else Absent or Ineligible, recorded
explicitly in `stringTier`. Length and Method sites get a string block in
front of the object path, sharing one tag extraction. A progress term on
the record (NotPrepared, a changed class ID, or an Absent name added in
place to a cacheable dictionary) drives the recompiles that keep the block
current.

**Tech Stack:** C++17, asmjit x86-64 and a64, lit/FileCheck (arm64 under
qemu-user), GoogleTest.

**Spec:** doc/superpowers/specs/2026-10-26-jit-getbyid-string-design.md

Read the spec in full before Task 1, and read `DESIGN-PRINCIPLES.md`
(repo root). Where this plan resolves an ambiguity in the spec it says so
in the task ("Spec resolution"); the list is also in the last section.

## Global Constraints

- Scope: x86-64 and arm64; HV64, HV32 and boxed. Only `GetById`,
  `GetByIdShort` and `GetByIdLong` sites with a read cache entry (the
  `recording_` sites that have a `JitByIdSiteRecord`) get a string block.
  Non-goals (spec "Non-goals"): calling the loaded method faster,
  properties inherited by `String.prototype`, accessors, other primitive
  receivers, `TryGetById*`, `GetByIdWithReceiver*`, sites with
  `cacheIdx == PROPERTY_CACHING_DISABLED`, the interpreter and StaticH.
- No interpreter or StaticH source changes are committed (Task 7's
  StaticH.cpp instrumentation is temporary and reverted). No bytecode or
  BytecodeVersion.h changes.
- `JitByIdSiteRecord` gains exactly these fields, with these comments,
  after `chainEntryEmitted`:
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
  and the enum, in this order: `NotPrepared`, `Absent`, `Ineligible`,
  `Length`, `Method` (spec "Recording", verbatim in Task 1).
- `prepareByIdString` has exactly the spec's signature:
  ```cpp
  JitByIdStringTier prepareByIdString(
      Runtime &runtime,
      JitByIdSiteRecord &site,
      SymbolID name,
      llvh::function_ref<uint16_t(HiddenClass *)> lazyId,
      SlotIndex &slot);
  ```
- The emitted layout is the spec's: `get_tag temp1, src; cmp STRING;
  jne objCheck; <string block>; objCheck: cmp OBJECT; jne slowPathLab;
  <today's object path from get_pointer on>`. Every other site emits
  exactly today's code.
- The new counter is `NumByIdStringHits`, appended to `JIT_COUNTERS`
  after `NumByIdChainHits`; only the emitted code increments it, only
  under `-Xjit-emit-counters`.
- Block comments: `// Get string length (site N)` and
  `// Get string method (site N, slot M)`, N the site's bytecode offset.
  Dump line, printed after `JIT ById sites: ...` only when L or M is
  non-zero: `JIT ById string sites: L length, M method`.
- `RuntimeOffsets::stringPrototype = offsetof(Runtime, stringPrototype)`
  with a `static_assert` that the field is 8 bytes.
- Every C++ change under `lib/VM/`, `include/hermes/VM/` or
  `unittests/VMRuntime/` is written after invoking the `gc-safe-coding`
  skill. Hermes throws by longjmp: no `LocalsRAII` or `GCScope` may span a
  call that can throw. Never hold a raw heap pointer across a GC point.
- 80 columns, 2-space indent, a doc comment on every declaration, only
  trivial one-line methods inside a class body. Before each commit, format
  only the lines the task changed:
  `git clang-format HEAD -- <the task's C++ files>`.
- Never bare `cd`; use absolute paths, `git -C`, or `( cd dir; cmd; )`.
  The shell's working directory is the repo root,
  `/home/tmikov/work/hermes-x86-jit`; commands below are relative to it.
- Build trees (already configured): `cmake-build-x86jit` (HV64 ASan
  Debug, `HERMESVM_ALLOW_JIT=2`, the reference tree),
  `cmake-build-x86jit-hv32` (HEAP_HV_PREFER32), `cmake-build-x86jit-boxed`
  (HEAP_HV_BOXED), `cmake-build-x86jit-hsan` (ASan, HV64, MallocGC +
  `HERMESVM_SANITIZE_HANDLES`), `cmake-build-host` (host `hermesc`/
  `shermes` for the cross builds), `cmake-build-arm64`,
  `cmake-build-arm64-hv32`, `cmake-build-arm64-boxed` (qemu-user), and
  `cmake-build-jit2` (Release, JIT=2, HV64) for benchmarks.
- Before building ANY arm64 tree, rebuild the host compilers:
  `cmake --build cmake-build-host --target hermesc shermes`.
- JIT suite on a tree: `LIT_FILTER='jit/' cmake --build <tree> --target
  check-hermes`. Full suite (lit and unit tests): the same without
  `LIT_FILTER`. One file group: `LIT_FILTER='getbyid-string'`. Rebuild a
  tree (`--target hermes`, plus `HermesVMRuntimeTests` when unit tests
  changed) before running its suite, and after every mutation and every
  restore.
- On `cmake-build-x86jit-hsan`, `jit/x86-64/counters-slow-call-kinds.js`
  is a known flake (dz `01a06156`); a failure of that test alone there is
  expected and is reported as such, not investigated.
- arm64 sanity: `./aarch64/qemu-sanity.sh <tree>` with the tree name
  relative to the repo root; the script builds `$ROOT/$BUILD/bin/hermes`
  (aarch64/qemu-sanity.sh:15-18), so an absolute path breaks it.
- Unit tests: `cmake --build <tree> --target HermesVMRuntimeTests`, then
  `<tree>/unittests/VMRuntime/HermesVMRuntimeTests
  --gtest_filter='<pattern>'`. They also run inside `check-hermes`.
- Prove-can-fail: every new test is shown to FAIL with the code it pins
  disabled (the mutation named in the task), then the code is restored,
  the tree REBUILT, and the test re-run green.
- `S=/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad`
  (the session scratchpad); every temporary file goes under `$S/str`,
  never in the repo.
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
  every suite run with its pass/unsupported/fail counts, and each mutation
  with the failure it produced. Task 7 copies these into "Delivered".
- The spec and this plan are committed before Task 1. Before Task 1,
  record `git rev-parse HEAD` as BASE in Task 1's report; Task 7
  benchmarks against BASE.

## Review Focus

Inputs the spec implies but its test list does not name, most likely
failure first, with the behavior a person would expect; each has a test
in the owning task:

1. A Length site whose result register is its source register
   (`s = s.length`): the block must read the source before it writes the
   result (`hwRes` is allocated last and may share the source's register,
   x86-64/JitEmitter-property.cpp:933-935). Task 3,
   getbyid-string-length.js, `lenSelf`, whose bytecode the BC RUN line
   pins as `GetById* rN, rN`.
2. A Method slot held directly in the object (no `propStorage`
   indirection) and one held out of line (the indirection rewrites the
   object register, which `hwRes` may share): both must read the right
   value. Task 3, getbyid-string-method.js, `ts` (`toString`, a direct
   slot) and `io` (`indexOf`, out of line), slots pinned in the DUMP lines
   and by Task 1's unit test.
3. On arm64, a lazy JIT ID of `String.prototype`'s class above 4095 that
   is not a multiple of 4096: the compare must materialize it in `w16`
   (`emit_cmp_imm32`, arm64/JitEmitter-internal.h:572-583) and still
   compare it. Task 4, getbyid-string-bigid.js.
4. Receivers whose tag is another pointer tag (BigInt), a non-pointer
   (symbol, number, boolean), or an object wrapping a string
   (`new String(...)`) at a site with a string block: none may take the
   block; each reads through the object path or the slow call. Task 5,
   getbyid-string-other-prim.js.
5. A throwing receiver (`null`, `undefined`) at a site with a string
   block: a TypeError, after which the block keeps hitting. Task 5,
   getbyid-string-other-prim.js.

---

## File Structure

- `include/hermes/VM/JIT/JitFunctionData.h` — `JitByIdStringTier`, the
  four record fields.
- `include/hermes/VM/JIT/JitCounters.h` — `NumByIdStringHits`.
- `lib/VM/JIT/RuntimeOffsets.h` — `stringPrototype`.
- `lib/VM/JIT/JitHandlers.{h,cpp}` — `prepareByIdString`,
  `byIdStringCanProgress`, the harvest.
- `lib/VM/JIT/JitCompiler.cpp` — carry-forward resets, progress, dump
  line.
- `lib/VM/JIT/x86-64/JitEmitter-property.cpp`,
  `lib/VM/JIT/arm64/JitEmitter-property.cpp` — `GetByIdImpl`: preparation
  call (Task 2), string block (Tasks 3 and 4).
- `unittests/VMRuntime/JitStringPrepTest.cpp` (new),
  `unittests/VMRuntime/CMakeLists.txt`.
- `test/jit/getbyid-string-*.js` (new), `test/jit/x86-64/exceptions.js`.
- `doc/JIT.md`, the spec's "Delivered" section, dz `01a16b01`.

---

### Test conventions (apply to every new lit test)

Process-wide counters (`NumRecompiles`, `NumByIdDemotions`,
`NumByIdChainHits`, `NumByIdStringHits`) see every function. A declared
global read inside a function is a recording GetById site, and every
PutById and PutByVal miss counts a decline of its own function. So every
new test:

- wraps its scenario in `function main() { ... } main();` (or
  `main(<arg>)`), every helper a local function inside `main`;
- passes the function under test and its data to drivers as parameters,
  so hot loops contain no global reads and no property access besides the
  site under test; drivers accumulate in locals, never store numbers into
  objects or arrays;
- keeps every function's own decline count under 16: `main` executes
  each of its GetById/PutById/PutByVal sites once (each a decline of
  main's cold body), fewer than 16 in total; `String.prototype` is read
  once into `SP`;
- each site under test is the only GetById of its name in its own
  function (spec "Attribution");
- runs in every JIT tree, x86-64 and arm64, all heap modes, and the hsan
  tree, with NO `UNSUPPORTED: handle_san` line (spec "Testing"). The only
  gate is the interim `REQUIRES: jit, jit-arch-x86-64` of Task 3's tests,
  lifted in Task 4. If a count differs on the hsan tree alone, find why
  (MallocGC collects at 1% of allocations and can clear weak caches of dead
  classes; recompile-getidx-demote-carried.js:12-15 records a MallocGC
  off-by-one in NumRecompileChecks). Find the cause; if it is a bug, fix
  it. If it is inherent to MallocGC, the binding spec forbids the gate:
  stop and report it to the controller, which asks the user. Never add a
  gate on your own.

Flags: `-fno-inline -Xjit=force -Xjit-crash-on-error
-Xjit-recompile-threshold=16` as in test/jit/getbyid-chain.js:8-10, plus
`-Xjit-max-recompiles=8` (the default budget is 2,
include/hermes/VM/RuntimeFlags.h:244-250) so that the budget never masks
a progress bug. A test that needs a scenario per process passes it as a
script argument (`-Xhermes-internal-test-methods %s -- a`, read with
`hermescli.getScriptArgs()[0]`, as test/jit/recompile-getidx-demote-carried.js:8-9
and :76 do; `hermescli` exists only with that flag,
lib/ConsoleHost/ConsoleHost.cpp:667-695). `gc()` is a global function
(lib/VM/JSLib/GlobalObject.cpp:950-951); tests pass it as a parameter.

Dump ordering (lib/VM/JIT/JitCompiler.cpp:581-588 and :707-753):
`-Xdump-jitcode=3` prints `JIT compilation of FunctionID <n>,
'<f>'[ (version N)]`, the code with its `//` comments, `JIT total memory
usage`, `JIT successfully compiled FunctionID <n>, '<f>'[ (version N)]`,
then the summary lines (`JIT cold ById sites`, `JIT ByVal sites`,
`JIT ById sites`, and after this plan `JIT ById string sites`). A
string-only site's cache stays cold (`_sh_ljs_get_by_id_rjs` never reads
or fills it for a primitive receiver, lib/VM/StaticH.cpp:1259-1272 and
lib/VM/Interpreter-slowpaths.cpp:1584-1610), so its version 2 prints
`JIT cold ById sites: 1` first; checks therefore use
`DUMP-NOT: JIT compilation of` followed by `DUMP: JIT ById string
sites: ...` to scope the summary to the function. Counters go to stderr
after `JIT counters:` (lib/ConsoleHost/ConsoleHost.cpp:1131-1134), one
`Name: value` line each in `JIT_COUNTERS` order
(lib/VM/JIT/x86-64/JIT.cpp:37-44), so counter RUN lines use `2>&1`.
Existing tests check counters by name with `CHECK:`, never `CHECK-NEXT`
after the last counter, so appending one changes none of them.

Every new test starts with the JS copyright header of
test/jit/count.js:1-6 and carries `// REQUIRES: jit`.

---

### Task 1: Records, offsets, counter and preparation

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h:262-301`
- Modify: `include/hermes/VM/JIT/JitCounters.h:40-43`
- Modify: `lib/VM/JIT/RuntimeOffsets.h:32`
- Modify: `lib/VM/JIT/JitCompiler.cpp:146-157`
- Modify: `lib/VM/JIT/JitHandlers.h:280-297`, `lib/VM/JIT/JitHandlers.cpp`
  (after `jitWalkChain`, :748)
- Create: `unittests/VMRuntime/JitStringPrepTest.cpp`
- Modify: `unittests/VMRuntime/CMakeLists.txt:45`

**Interfaces:**
- Produces: `enum class JitByIdStringTier : uint8_t { NotPrepared,
  Absent, Ineligible, Length, Method }`; `JitByIdSiteRecord::sawString`,
  `stringTier`, `stringProtoId`, `stringProtoCount`;
  `JitCounter::NumByIdStringHits`;
  `static constexpr uint32_t RuntimeOffsets::stringPrototype`;
  `JitByIdStringTier prepareByIdString(Runtime &, JitByIdSiteRecord &,
  SymbolID, llvh::function_ref<uint16_t(HiddenClass *)>, SlotIndex &)`.

Facts this task relies on:
- `Runtime::stringPrototype` is a public `PinnedValue<JSString>`
  (include/hermes/VM/RuntimeHermesValueFields.def:25, expanded at
  include/hermes/VM/Runtime.h:929); `PinnedValue` privately inherits
  `PinnedHermesValue` (include/hermes/VM/Handle.h:200) and has
  `getHermesValue()` (:265-267), so the field is a full 64-bit HermesValue
  in every heap mode. `RuntimeOffsets` is a friend of `Runtime`
  (Runtime.h:1344). `Handle<JSObject>::vmcast(const PinnedValue<U> *)`
  exists (Handle.h:544).
- `HiddenClass::getLazyJITId()`/`setLazyJITId()` are public under
  `HERMESVM_JIT` (HiddenClass.h:458-465); `getNumProperties()` (:477),
  `isDictionary()` (:487), `isDictionaryNoCache()` (:530);
  `kDictionaryThreshold` is 64 and public (:406-410).
- `isEligibleByIdShape(clazz, nullptr)` is `!clazz->isDictionaryNoCache()`
  (JitHandlers.cpp:542-546); `chainClassLookup` (JitHandlers.cpp:682-700)
  is a `static` non-allocating lookup defined after `prepareByIdPolyChain`
  (:620), so `prepareByIdString` goes after `jitWalkChain` (:702-748) in
  the .cpp (Spec resolution 1); its declaration sits next to
  `prepareByIdPolyChain`'s in the header.
- `Emitter::initHCLazyIDMayAlloc`, the production `lazyId`, roots the
  class in a `Locals` before allocating (x86-64/JitEmitter.cpp:713-717,
  :753), runs no JS, never throws (it swallows OOM, :721-727, :740-748),
  and is the only GC point in preparation; the arm64 copy starts at
  arm64/JitEmitter.cpp:672.
- The record is copied whole on carry-forward (`make_unique<...>(*s)`,
  JitCompiler.cpp:147) and the emitted-tier fields are reset one by one
  (:148-155).
- `String.prototype` is a `JSString` (lib/VM/JSLib/GlobalObject.cpp:465-473)
  holding `length` (PrimitiveBox.cpp:85-101) and about 45 more properties
  from lib/VM/JSLib/String.cpp:46-353; the unit test below pins that its
  initial class is not a dictionary and has room for the at most 10
  properties any lit test adds before the dictionary threshold
  (HiddenClass.cpp:499-500).

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Write the failing unit tests**

Create `unittests/VMRuntime/JitStringPrepTest.cpp`:

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

using JitStringPrepTest = RuntimeTestFixture;

/// \return the symbol for \p s. Its handle stays in the current GCScope,
/// which keeps the symbol alive.
SymbolID sym(Runtime &runtime, const char *s) {
  return **runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef(s));
}

/// \return String.prototype's current hidden class. A raw pointer, valid
/// until the next allocation.
HiddenClass *protoClass(Runtime &runtime) {
  return vmcast<JSObject>(runtime.stringPrototype.getHermesValue())
      ->getClass(runtime);
}

/// \return the slot of \p name, which must be an own data property of
/// String.prototype, as the object's own lookup reports it.
SlotIndex ownSlot(Runtime &runtime, SymbolID name) {
  NamedPropertyDescriptor desc;
  bool found = JSObject::getOwnNamedDescriptor(
      Handle<JSObject>::vmcast(&runtime.stringPrototype),
      runtime,
      name,
      desc);
  EXPECT_TRUE(found);
  EXPECT_FALSE(desc.flags.accessor);
  return desc.slot;
}

/// Run \p code as a script; it must not throw.
void run(Runtime &runtime, const char *code) {
  GCScopeMarkerRAII marker{runtime};
  hermes::hbc::CompileFlags flags;
  ASSERT_EQ(
      ExecutionStatus::RETURNED, runtime.run(code, "", flags).getStatus());
}

/// A lazyId callback that assigns IDs from 100 up, as
/// Emitter::initHCLazyIDMayAlloc would but without allocating, and counts
/// its calls.
struct FakeIds {
  /// The next ID to assign.
  uint16_t next = 100;
  /// How many times the callback ran.
  unsigned calls = 0;
  /// Assign \p hc an ID if it has none. \return its ID.
  uint16_t operator()(HiddenClass *hc) {
    ++calls;
    if (!hc->getLazyJITId())
      hc->setLazyJITId(next++);
    return hc->getLazyJITId();
  }
};

// `length` is decided by name alone: Length, no lazy ID requested, the
// slot untouched. Fails if the name check is missing: String.prototype
// owns a data property `length` (PrimitiveBox.cpp:85-101), so the lookup
// would give Method.
TEST_F(JitStringPrepTest, Length) {
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 12345;
  EXPECT_EQ(
      JitByIdStringTier::Length,
      prepareByIdString(
          runtime,
          site,
          Predefined::getSymbolID(Predefined::length),
          ids,
          slot));
  EXPECT_EQ(JitByIdStringTier::Length, site.stringTier);
  EXPECT_EQ(0u, ids.calls);
  EXPECT_EQ(0u, (unsigned)site.stringProtoId);
  EXPECT_EQ(12345u, slot);
}

// The initial String.prototype: not a dictionary, with room for the 10
// properties the lit tests add at most; toString in a direct slot and
// indexOf out of line (review focus 2 relies on both); charCodeAt gives
// Method at the slot the object's own lookup reports, with the class's
// new ID. Fails if found data properties are not Method or the slot is
// not the class's.
TEST_F(JitStringPrepTest, MethodCharCodeAt) {
  ASSERT_FALSE(protoClass(runtime)->isDictionary());
  EXPECT_LE(
      protoClass(runtime)->getNumProperties() + 10,
      HiddenClass::kDictionaryThreshold);
  EXPECT_LT(
      ownSlot(runtime, Predefined::getSymbolID(Predefined::toString)),
      (SlotIndex)HERMESVM_DIRECT_PROPERTY_SLOTS);
  EXPECT_GE(
      ownSlot(runtime, Predefined::getSymbolID(Predefined::indexOf)),
      (SlotIndex)HERMESVM_DIRECT_PROPERTY_SLOTS);
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  SymbolID cc = Predefined::getSymbolID(Predefined::charCodeAt);
  EXPECT_EQ(
      JitByIdStringTier::Method,
      prepareByIdString(runtime, site, cc, ids, slot));
  EXPECT_EQ(JitByIdStringTier::Method, site.stringTier);
  EXPECT_EQ(ownSlot(runtime, cc), slot);
  EXPECT_EQ(1u, ids.calls);
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
  EXPECT_EQ(100u, (unsigned)protoClass(runtime)->getLazyJITId());
}

// A name String.prototype's class does not own: Absent, with the class's
// ID and property count. Fails if the count is not recorded.
TEST_F(JitStringPrepTest, Absent) {
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Absent,
      prepareByIdString(
          runtime, site, sym(runtime, "noSuchStringProp"), ids, slot));
  EXPECT_NE(0u, site.stringProtoCount);
  EXPECT_EQ(protoClass(runtime)->getNumProperties(), site.stringProtoCount);
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
}

// An accessor of String.prototype: Ineligible, with the class's ID.
// Fails if accessors are taken as Method.
TEST_F(JitStringPrepTest, AccessorIsIneligible) {
  run(runtime,
      "Object.defineProperty(String.prototype, 'acc', "
      "{get: function () { return 1; }, configurable: true});");
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Ineligible,
      prepareByIdString(runtime, site, sym(runtime, "acc"), ids, slot));
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
}

// No lazy ID available: Ineligible with ID 0, even for a data property.
// Fails if a zero ID is not checked.
TEST_F(JitStringPrepTest, NoIdIsIneligible) {
  auto noId = [](HiddenClass *) -> uint16_t { return 0; };
  JitByIdSiteRecord site;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Ineligible,
      prepareByIdString(
          runtime,
          site,
          Predefined::getSymbolID(Predefined::charCodeAt),
          noId,
          slot));
  EXPECT_EQ(0u, (unsigned)site.stringProtoId);
}

// A no-cache dictionary String.prototype (a second deletion,
// HiddenClass.cpp:425-434) is not eligible: Ineligible, with the class's
// ID. Fails if eligibility is not checked.
TEST_F(JitStringPrepTest, NoCacheDictionaryIsIneligible) {
  run(runtime,
      "String.prototype.t1 = 1; delete String.prototype.t1; "
      "String.prototype.t2 = 1; delete String.prototype.t2;");
  ASSERT_TRUE(protoClass(runtime)->isDictionaryNoCache());
  JitByIdSiteRecord site;
  FakeIds ids;
  SlotIndex slot = 0;
  EXPECT_EQ(
      JitByIdStringTier::Ineligible,
      prepareByIdString(
          runtime,
          site,
          Predefined::getSymbolID(Predefined::charCodeAt),
          ids,
          slot));
  EXPECT_EQ(100u, (unsigned)site.stringProtoId);
}

// The lazyId callback is preparation's only GC point (spec: "GC during
// preparation"). This one collects before assigning. On the hsan tree
// (MallocGC + handle sanitization) the collection moves String.prototype's
// class and frees the old copy (MallocGC.cpp:65-102), so a class pointer
// held across the callback is a use after free that ASan reports. The
// callback roots its argument as initHCLazyIDMayAlloc does.
TEST_F(JitStringPrepTest, CollectingLazyId) {
  unsigned calls = 0;
  auto collecting = [this, &calls](HiddenClass *hc) -> uint16_t {
    ++calls;
    struct : public Locals {
      PinnedValue<HiddenClass> hc;
    } lv;
    LocalsRAII lraii{runtime, &lv};
    lv.hc = hc;
    runtime.collect("JitStringPrepTest");
    lv.hc->setLazyJITId(77);
    return 77;
  };
  JitByIdSiteRecord site;
  SlotIndex slot = 0;
  SymbolID cc = Predefined::getSymbolID(Predefined::charCodeAt);
  EXPECT_EQ(
      JitByIdStringTier::Method,
      prepareByIdString(runtime, site, cc, collecting, slot));
  EXPECT_EQ(1u, calls);
  EXPECT_EQ(77u, (unsigned)site.stringProtoId);
  EXPECT_EQ(77u, (unsigned)protoClass(runtime)->getLazyJITId());
  EXPECT_EQ(ownSlot(runtime, cc), slot);
}

} // namespace

#endif // HERMESVM_JIT
```

Add `JitStringPrepTest.cpp` to `RTSources` in
`unittests/VMRuntime/CMakeLists.txt`, on its own line after
`JitChainWalkTest.cpp` (line 45).

- [ ] **Step 3: See them fail**

```bash
cmake --build cmake-build-x86jit --target HermesVMRuntimeTests
```

Expected: compile errors, `JitByIdStringTier` and `prepareByIdString`
undeclared.

- [ ] **Step 4: The enum and the record fields**

In `include/hermes/VM/JIT/JitFunctionData.h`, before the doc comment of
`struct JitByIdSiteRecord` (line 262), add:

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

In the record's doc comment, replace "`_jit_get_by_id records observed
shapes and sets seen and changed.`" with "`_jit_get_by_id records
observed shapes and sets seen, changed and sawString.`". After
`chainEntryEmitted` (line 295) add the four fields exactly as in Global
Constraints.

- [ ] **Step 5: The counter**

In `include/hermes/VM/JIT/JitCounters.h`, replace the last entry
(lines 41-43) with:

```cpp
  /* Inline hits of emitted GetById chain entries, counted by the     \
   * emitted code only with -Xjit-emit-counters. */                   \
  X(NumByIdChainHits)                                                 \
  /* Inline hits of the GetById string blocks (Length and Method),    \
   * counted by the emitted code only with -Xjit-emit-counters. */    \
  X(NumByIdStringHits)
```

(Re-align the backslashes of the whole macro if `git clang-format` asks.)

- [ ] **Step 6: The offset**

In `lib/VM/JIT/RuntimeOffsets.h`, after `globalObject` (line 32) add:

```cpp
  /// Runtime::stringPrototype, a PinnedValue<JSString>: a full 64-bit
  /// HermesValue in every heap mode. GetById string Method blocks reload
  /// it on every execution, so they follow String.prototype when it moves.
  static constexpr uint32_t stringPrototype =
      offsetof(Runtime, stringPrototype);
  static_assert(
      sizeof(Runtime::stringPrototype) == 8,
      "string Method blocks load String.prototype as one 64-bit value");
  static_assert(
      stringPrototype % 8 == 0,
      "arm64's emit_load_from_base_offset<8> needs an 8-aligned offset");
```

- [ ] **Step 7: Carry-forward resets**

In `lib/VM/JIT/JitCompiler.cpp`, in the ById carry-forward loop, after
`r->chainEntryEmitted = 0;` (line 151) add:

```cpp
          r->stringTier = JitByIdStringTier::NotPrepared;
          r->stringProtoId = 0;
          r->stringProtoCount = 0;
```

`sawString` carries (observed tier), like `seen`.

- [ ] **Step 8: Declare the preparation**

In `lib/VM/JIT/JitHandlers.h`, after the `prepareByIdPolyChain`
declaration (line 297) add:

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

- [ ] **Step 9: Implement it**

In `lib/VM/JIT/JitHandlers.cpp`, after `jitWalkChain` (line 748) add:

```cpp

/// \return String.prototype's current hidden class. A raw pointer, valid
/// only until the next GC point. Never allocates.
static HiddenClass *stringProtoClass(Runtime &runtime) {
  return vmcast<JSObject>(runtime.stringPrototype.getHermesValue())
      ->getClass(runtime);
}

JitByIdStringTier prepareByIdString(
    Runtime &runtime,
    JitByIdSiteRecord &site,
    SymbolID name,
    llvh::function_ref<uint16_t(HiddenClass *)> lazyId,
    SlotIndex &slot) {
  site.stringProtoId = 0;
  site.stringProtoCount = 0;
  // 1. The only own property of a primitive string a GetById can name
  // (Lowering.cpp:240-256 turns index names into numbers).
  if (name == Predefined::getSymbolID(Predefined::length)) {
    site.stringTier = JitByIdStringTier::Length;
    return site.stringTier;
  }
  // 2. The only GC point: the class is read immediately before it, and
  // no raw pointer is held across it.
  uint16_t id = lazyId(stringProtoClass(runtime));
  // 3. Re-read the class; nothing below allocates.
  NoAllocScope noAlloc(runtime);
  HiddenClass *clazz = stringProtoClass(runtime);
  site.stringProtoId = id;
  NamedPropertyDescriptor desc;
  if (id == 0 || !isEligibleByIdShape(clazz, nullptr)) {
    site.stringTier = JitByIdStringTier::Ineligible;
  } else if (!chainClassLookup(runtime, clazz, name, desc)) {
    site.stringTier = JitByIdStringTier::Absent;
    site.stringProtoCount = clazz->getNumProperties();
  } else if (desc.flags.accessor) {
    site.stringTier = JitByIdStringTier::Ineligible;
  } else {
    site.stringTier = JitByIdStringTier::Method;
    slot = desc.slot;
  }
  return site.stringTier;
}
```

- [ ] **Step 10: Run the tests**

```bash
cmake --build cmake-build-x86jit --target hermes HermesVMRuntimeTests
cmake-build-x86jit/unittests/VMRuntime/HermesVMRuntimeTests \
  --gtest_filter='JitStringPrepTest.*'
cmake --build cmake-build-x86jit-hsan --target HermesVMRuntimeTests
cmake-build-x86jit-hsan/unittests/VMRuntime/HermesVMRuntimeTests \
  --gtest_filter='JitStringPrepTest.*'
```

Expected: all 7 PASS on both trees. If `MethodCharCodeAt`'s slot
expectations fail (toString not direct or indexOf direct), STOP and
report the slots: Task 3's review-focus test depends on them.

- [ ] **Step 11: Prove they can fail**

One at a time, rebuilding `HermesVMRuntimeTests` after each change and
each restore:
- Mutation A: delete the `length` early return → `Length` FAILS
  (Method).
- Mutation B: delete the `else if (desc.flags.accessor)` branch →
  `AccessorIsIneligible` FAILS.
- Mutation C: replace `id == 0 || ` with nothing → `NoIdIsIneligible`
  FAILS (Method with ID 0).
- Mutation D: replace `!isEligibleByIdShape(clazz, nullptr)` with
  `false` → `NoCacheDictionaryIsIneligible` FAILS.
- Mutation E: delete `site.stringProtoCount = clazz->getNumProperties();`
  → `Absent` FAILS.
- Mutation F, on cmake-build-x86jit-hsan: hold the class across the
  callback: replace the two lines from `uint16_t id = lazyId(...)` through
  `HiddenClass *clazz = stringProtoClass(runtime);` with
  ```cpp
  HiddenClass *clazz = stringProtoClass(runtime);
  uint16_t id = lazyId(clazz);
  NoAllocScope noAlloc(runtime);
  ```
  → `CollectingLazyId` FAILS with an ASan heap-use-after-free report. If
  it passes, the collection did not move the class: report it; the
  spec's GC claim then needs another test.

- [ ] **Step 12: Other trees build; JIT suite**

```bash
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
```

Expected: the JIT suite passes (the counter adds a line nobody checks;
nothing reads the new fields yet); the arm64 tree builds.

- [ ] **Step 13: Commit**

```bash
git add include/hermes/VM/JIT/JitFunctionData.h \
  include/hermes/VM/JIT/JitCounters.h lib/VM/JIT/RuntimeOffsets.h \
  lib/VM/JIT/JitCompiler.cpp lib/VM/JIT/JitHandlers.h \
  lib/VM/JIT/JitHandlers.cpp unittests/VMRuntime/JitStringPrepTest.cpp \
  unittests/VMRuntime/CMakeLists.txt
git commit   # "JIT: GetById string records and preparation"
```

---

### Task 2: Harvest, progress, dump, and preparation in the emitters

The state machine without emission: sites learn `sawString`, the
NotPrepared term recompiles them once, both emitters prepare them, and
the dump shows what was prepared. Interim (until Tasks 3 and 4): Length
and Method sites record their tier but emit nothing, so their strings keep
declining; Length never progresses again, Method only on a class change,
so such sites demote as today and nothing loops.

**Files:**
- Modify: `lib/VM/JIT/JitHandlers.h` (after the Task 1 declaration),
  `lib/VM/JIT/JitHandlers.cpp:750-766` (harvest), and after Task 1's
  `prepareByIdString`
- Modify: `lib/VM/JIT/JitCompiler.cpp:315-334` (progress), `:453-470`
  and `:473-512` (callers), `:734-752` (dump)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp:727-744` (members),
  `:781-789` (run); `lib/VM/JIT/arm64/JitEmitter-property.cpp:831-848`,
  `:885-893`
- Modify: `test/jit/x86-64/exceptions.js:365-394`
- Test: `test/jit/getbyid-string-absent.js`,
  `test/jit/getbyid-string-hcid.js`

**Interfaces:**
- Consumes: Task 1.
- Produces: `bool byIdStringCanProgress(Runtime &, const
  JitByIdSiteRecord &)`; JitCompiler.cpp's
  `byIdSiteCanProgress(Runtime &, const JitByIdSiteRecord &)` and
  `byIdShapeProgress(Runtime &, const JitVersionData *)`; emitter members
  `uint32_t siteId_`, `JitByIdStringTier strTier_`, `SlotIndex strSlot_`.

Facts: the harvest runs only while the body is current and the slot
still holds `_jit_get_by_id` (JitHandlers.cpp:1002), after the forwarded
lookup, so a throwing lookup is not harvested (it longjmps past,
:987-990). Its return value only gates the chain walk, which needs an
object (:1006-1010). `considerRecompile` has `Runtime &runtime`
(JitCompiler.cpp:411); `byIdSiteCanProgress` is called by the demotion
pass (:456) and by `byIdShapeProgress` (:331), which scans every record,
demoted ones included (:330-332). A demoted site is skipped by the
demotion pass (:454-455). The emitter computes the site's bytecode offset
in `run()` (x86 :782-783, arm64 :886-887) before emitting any code; the
planning phase that needs the tier starts in `emitFastPath` (x86
:898-935, arm64 :1006-1043).

**Spec resolution 2.** The string progress predicate is a function in
JitHandlers.cpp, `byIdStringCanProgress`, called by `byIdSiteCanProgress`:
it reads `String.prototype`'s class, which JitHandlers.cpp already does
for preparation.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Write the failing tests**

`test/jit/getbyid-string-absent.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// Sites whose name String.prototype's class does not own: `foo` exists
// nowhere, `hasOwnProperty` is inherited from Object.prototype. Each
// site's version 1 is compiled before any string arrives (NotPrepared).
// Calls 1-16 take the recording helper; the first sets sawString (and
// changed) and the 16th crosses the threshold, where the NotPrepared term
// recompiles. Version 2 prepares the site Absent and emits nothing, so
// calls 17-100 still decline. Its crossings at calls 32, 48 and 64 find
// the same class ID, a String.prototype that is not a dictionary, and
// nothing changed, so the third demotes the site to the plain helper;
// calls 65-100 count nothing. Per site: one recompile, one demotion.
// main() runs two recording reads once each (2 declines); count() none.
// Fails before the feature and without the NotPrepared term
// (NumRecompiles 0), if `changed` were set for every string receiver
// (NumByIdDemotions 0: every crossing would see a change), and if Absent
// were progress (NumRecompiles above 2).
function main() {
  function getFoo(s) {
    return s.foo;
  }
  function getHas(s) {
    return s.hasOwnProperty;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  var has = Object.prototype.hasOwnProperty;
  print(count(getFoo, 'abc', 100, undefined), count(getHas, 'abc', 100, has));
}
main();

// OUT: 100 100
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 2{{$}}
// CNT: NumByIdStringHits: 0{{$}}
```

`test/jit/getbyid-string-hcid.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-hc-id-limit=0 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-hc-id-limit=0 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// No lazy JIT ID can be assigned (-Xjit-hc-id-limit=0; JitEmitter.cpp
// returns 0 once prevHCId reaches the limit), so a charCodeAt site is
// prepared Ineligible with stringProtoId 0. Call 16 crosses in version 1
// (NotPrepared: one recompile). Version 2 emits nothing and keeps
// declining; at calls 32, 48 and 64 String.prototype's class still reads
// ID 0, equal to the stored 0, so nothing progresses and the third
// crossing demotes the site. Calls 65-160 go through the plain helper.
// NumRecompiles = 1, NumByIdDemotions = 1.
// Fails before the feature (NumRecompiles 0), and if Ineligible were
// progress regardless of IDs (a recompile at every crossing up to the
// budget of 8).
function main() {
  function getCC(s) {
    return s.charCodeAt(0);
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(getCC, 'abc', 160, 97));
}
main();

// OUT: 160
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdStringHits: 0{{$}}
```

Both stay valid after Tasks 3 and 4: Absent and Ineligible sites emit
nothing.

- [ ] **Step 3: See them fail**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-string' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: both CNT runs FAIL (`NumRecompiles: 0`); the OUT runs pass.

- [ ] **Step 4: The harvest**

In `lib/VM/JIT/JitHandlers.cpp`, `recordByIdObservation`: replace its
doc comment's `\return` lines (752-753) with

```cpp
/// \return true if the read cache described the receiver as an own or
///   prototype hit whose shape was offered to addByIdShape, or if this is
///   the site's first string receiver (nothing to walk either way).
```

and replace lines 764-766

```cpp
  HermesValue src = *toPHV(source);
  if (!src.isObject())
    return false;
```

with

```cpp
  HermesValue src = *toPHV(source);
  // A string receiver marks the site once; later ones change nothing
  // (spec: "Recording"; flags mark transitions, not repeats).
  if (src.isString() && !site.sawString) {
    site.sawString = 1;
    site.changed = 1;
    return true;
  }
  if (!src.isObject())
    return false;
```

- [ ] **Step 5: The progress predicate**

In `lib/VM/JIT/JitHandlers.h`, after `prepareByIdString`'s declaration:

```cpp

/// \return true if \p site's string receivers give a recompile something
/// to do (spec: "Progress"): it saw a string and its body did not prepare
/// it, or it prepared Method, Absent or Ineligible against a
/// String.prototype class whose lazy JIT ID is not the current one, or it
/// prepared Absent against a cacheable dictionary that has since gained a
/// property in place. Never allocates.
bool byIdStringCanProgress(Runtime &runtime, const JitByIdSiteRecord &site);
```

In `lib/VM/JIT/JitHandlers.cpp`, after `prepareByIdString`:

```cpp

bool byIdStringCanProgress(Runtime &runtime, const JitByIdSiteRecord &site) {
  if (!site.sawString)
    return false;
  if (site.stringTier == JitByIdStringTier::NotPrepared)
    return true;
  if (site.stringTier == JitByIdStringTier::Length)
    return false;
  NoAllocScope noAlloc(runtime);
  const HiddenClass *clazz = stringProtoClass(runtime);
  // Reads the class's ID without assigning one: 0 if it has none.
  if (clazz->getLazyJITId() != site.stringProtoId)
    return true;
  // A cacheable dictionary adds properties in place, keeping its class
  // (HiddenClass.cpp:455-482); while it stays current its count only
  // grows, so a different count means an addition.
  return site.stringTier == JitByIdStringTier::Absent &&
      clazz->isDictionary() && !clazz->isDictionaryNoCache() &&
      clazz->getNumProperties() != site.stringProtoCount;
}
```

In `lib/VM/JIT/JitCompiler.cpp`, replace `byIdSiteCanProgress` and
`byIdShapeProgress` (lines 315-334) with:

```cpp
/// \return true if \p s holds a live shape that the owning body did not
/// emit and that a compile could still emit, or if its string receivers
/// can progress (byIdStringCanProgress).
static bool byIdSiteCanProgress(
    Runtime &runtime,
    const JitByIdSiteRecord &s) {
  for (const auto &e : s.entries)
    if (e.isLive() && !e.emitted && !e.unemittable)
      return true;
  return byIdStringCanProgress(runtime, s);
}

/// \return true if some GetById site can progress (spec: "Progress
/// term"; 2026-10-26 string spec: "Progress").
static bool byIdShapeProgress(Runtime &runtime, const JitVersionData *vd) {
  const JitConsumerRecords *cr = vd->consumerRecords.get();
  if (!cr)
    return false;
  for (const auto &s : llvh::make_pointee_range(cr->byIdSites))
    if (byIdSiteCanProgress(runtime, s))
      return true;
  return false;
}
```

At line 456 pass `runtime`:
`bool canProgress = actionablePossible && byIdSiteCanProgress(runtime, s);`
and at line 510: `progress = byIdShapeProgress(runtime, vd);`. In the
comment at :475-479 replace "or a GetById site holding a live shape the
current body did not emit" with "or a GetById site holding a live shape
the current body did not emit or string state it did not prepare for".

- [ ] **Step 6: The dump line**

In `lib/VM/JIT/JitCompiler.cpp`, after the `if (recorded) { ... }` block
(line 752), still inside `if (auto *cr = ...)`, add:

```cpp
      // String receivers: sites this body prepared with a string block.
      // A separate line, so existing `JIT ById sites` checks keep matching.
      size_t strLength = 0, strMethod = 0;
      for (const auto &s : llvh::make_pointee_range(cr->byIdSites)) {
        strLength += s.stringTier == JitByIdStringTier::Length;
        strMethod += s.stringTier == JitByIdStringTier::Method;
      }
      if (strLength || strMethod) {
        llvh::outs() << "JIT ById string sites: " << strLength
                     << " length, " << strMethod << " method\n";
      }
```

- [ ] **Step 7: Preparation in both emitters**

In `lib/VM/JIT/x86-64/JitEmitter-property.cpp`, `GetByIdImpl`, after the
`site_` member (line 732) add:

```cpp
  /// The instruction's bytecode offset; set together with site_.
  uint32_t siteId_ = 0;
  /// What prepareByIdString() prepared for string receivers here;
  /// NotPrepared if it was not called (no record, or no string seen).
  JitByIdStringTier strTier_ = JitByIdStringTier::NotPrepared;
  /// Method only: the property's slot in String.prototype.
  SlotIndex strSlot_ = 0;
```

and replace lines 781-789 (the `if (recording_ && ...)` block of `run()`)
with:

```cpp
    if (recording_ && cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
      siteId_ = (uint32_t)((const char *)_.emittingIP -
                           (const char *)_.codeBlock_->begin());
      site_ = &_.byIdSiteRecord(siteId_);
      // Only a never-flipped slot is initialized; a demoted value
      // carried forward stays demoted.
      if (!site_->helper)
        site_->helper = (void *)_jit_get_by_id;
      // String receivers (2026-10-26 string spec, "Preparation"): before
      // any code for this instruction, since a Length block needs a
      // vector temporary in the planning phase. A GC point.
      if (site_->sawString) {
        strTier_ = prepareByIdString(
            _.runtime_,
            *site_,
            SymbolID::unsafeCreate(symID),
            [this](HiddenClass *hc) { return _.initHCLazyIDMayAlloc(hc); },
            strSlot_);
      }
    }
```

Make the same two edits in `lib/VM/JIT/arm64/JitEmitter-property.cpp`
(members after line 836, `run()` block at lines 885-893); the code is
identical. Nothing in `GetByIdImpl` holds a heap pointer across the call:
`site_` and the read cache entry are malloc'd, and a GC here is already
possible from `prepareByIdPolyChain` (x86 :952-958).

- [ ] **Step 8: Update exceptions.js**

`test/jit/x86-64/exceptions.js`'s `churn` reads `o.c.length` on a string
in its catch handler, 125 times (:351-364); its version 2 now prepares
that site Length,
so the summary gains a line after `JIT ById sites` and the
`--match-full-lines` `CHECK-NEXT: 1999679` lines (:386, :394) break. Run

```bash
cmake --build cmake-build-x86jit --target hermes
cmake-build-x86jit/bin/hermes -fno-inline -Xjit=force -Xdump-jitcode=2 \
  test/jit/x86-64/exceptions.js | grep -A3 "'churn' (version 2)"
cmake-build-x86jit/bin/hermes -fno-inline -O0 -Xjit=force -Xdump-jitcode=2 \
  test/jit/x86-64/exceptions.js | grep -A3 "'churn' (version 2)"
```

Expected in both: `JIT ById string sites: 1 length, 0 method` right
after the `JIT ById sites:` line (churn has one string GetById). Insert
`// CHECK-NEXT: JIT ById string sites: 1 length, 0 method` right after
the `// CHECK-NEXT: JIT ById sites: ...` line (385), and
`// CHECK0-NEXT: JIT ById string sites: 1 length, 0 method` right after
the `// CHECK0-NEXT: JIT ById sites: ...` line (393 before the first
insertion). If either run prints anything else, find the site before
changing the expectation. In the comment (:365-378) replace "`o.c.length`
reads a string primitive and records none" with "`o.c.length` reads a
string primitive: version 2 prepares it as a Length site, which never
progresses again", and add, after "either makes version 2 compile", "and
so does the string site's NotPrepared term".

- [ ] **Step 9: Run**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-string|exceptions' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all PASS. A count that differs from its derivation is
investigated before any expectation changes (a run with
`-Xdump-jitcode=3` shows which versions compiled).

- [ ] **Step 10: Prove they can fail**

One at a time, rebuild after each change and each restore:
- Mutation A: in `byIdStringCanProgress`, `return false;` for
  NotPrepared → absent.js FAILS (`NumRecompiles: 0`), hcid.js FAILS.
- Mutation B: in `byIdStringCanProgress`, return true for
  `JitByIdStringTier::Ineligible` before the ID compare → hcid.js FAILS
  (`NumRecompiles` above 1).
- Mutation C: in the harvest, drop `&& !site.sawString` (the flag marks
  repeats) → absent.js FAILS (`NumByIdDemotions: 0`).

- [ ] **Step 11: Suites**

```bash
cmake --build cmake-build-x86jit --target check-hermes
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes &&
  LIT_FILTER='jit/' cmake --build cmake-build-arm64 --target check-hermes ||
  echo "FAIL arm64"
```

Expected: no failure. Any GetById site in an existing test that sees a
string now spends one recompile at its first crossing (NotPrepared); if
an existing test's count or dump changes, find the string site, explain
the change in that test's comment, and update its expectation by path
in this task's commit.

- [ ] **Step 12: Commit**

```bash
git add lib/VM/JIT/JitHandlers.h lib/VM/JIT/JitHandlers.cpp \
  lib/VM/JIT/JitCompiler.cpp lib/VM/JIT/x86-64/JitEmitter-property.cpp \
  lib/VM/JIT/arm64/JitEmitter-property.cpp test/jit/x86-64/exceptions.js \
  test/jit/getbyid-string-absent.js test/jit/getbyid-string-hcid.js
git commit   # "JIT: record string receivers at GetById sites"
```

(Add any other test file Step 11 changed, by path.)

---

### Task 3: x86-64 string blocks

**Files:**
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp:918-941`
  (`emitFastPath` planning and the object check), new `emitStringBlock`
  after `emitFastPath` (before line 1052)
- Test: `test/jit/getbyid-string-length.js`,
  `getbyid-string-method.js`, `getbyid-string-mixed.js`,
  `getbyid-string-replace.js`, `getbyid-string-gc.js`

**Interfaces:**
- Consumes: Tasks 1 and 2 (`strTier_`, `strSlot_`, `siteId_`,
  `site_->stringProtoId`, `RuntimeOffsets::stringPrototype`,
  `NumByIdStringHits`).
- Produces: x86 `void emitStringBlock(const x86::Gp &src, HWReg hwVTmp)`.

**Register and label contract** (DESIGN-PRINCIPLES.md 5), against the
code as it stands:
- Planning (:894-935) emits no code. `freeAllFRTempExcept(frSource)`
  (:899) frees every FR temp but the source's, GP and vector
  (JitEmitter-regalloc.cpp:251-272); `getOrAllocFRInGpX(frSource, true)`
  (:902) puts the source in a GP register; `temp1` is allocated while the
  source is held (:912), so it never aliases the source; `freeFRTemp
  (frSource)` (:916) frees the source's GP and vector temps
  (JitEmitter-regalloc.cpp:274-292); `temp2`-`temp4` (:919-924) may alias
  the source's register; `hwRes` is allocated last (:935) and may alias
  any temp or the source (with `frRes == frSource` it is the same FR).
  The Length block's vector temporary is allocated after `temp4` and
  freed with the GP temps: no vector temp holds an FR then, so
  `_allocTemp` (JitEmitter.h:2457-2469) finds a free register and never
  spills, i.e. emits nothing.
- Entry to the string block: the `jne objCheckLab` after
  `emit_sh_ljs_is_string(a, temp1, src)` (get-tag + compare,
  JitEmitter-internal.h:137-143; the compare leaves `temp1` intact,
  :113-119) fell through, so `temp1` = tag, the source register intact.
- Inside: the Length block reads the source only in its first
  instruction (`emit_stringprim_get_length_and_flags`, whose
  `mov temp1, src` is the only read, JitEmitter-internal.h:954-962, :196-197)
  and writes `hwRes` last (`vmovq`); the Method block never reads the
  source and writes `hwRes` only past its guard (`emitLoadFromSlot`, which
  may rewrite `temp2` when the slot is out of line, :883-885, with `hwRes`
  possibly equal to `temp2`, which is fine: the load is the last use of
  `temp2`). `emitIncrementCounter` clobbers `r11` and EFLAGS
  (JitEmitter.cpp:587-603), neither live where it is placed. The boxed
  decoder clobbers `r11` (JitEmitter-internal.h:530-538), dead there.
- `contLab`: both blocks end in `jmp contLab` with the result in
  `hwRes`, as every object hit does; the slow path reaches it by fall-through
  after `movHWFromHW(hwRes, rax)` (:855-859). No block falls through.
- `slowPathLab`: the Method block's guard jumps there having written only
  `temp2`, `temp3` and EFLAGS. The slow call reads the source from the
  frame (synced at :793-794), loads its arguments into fixed registers and
  uses `r11` itself (:809-850): it reads no temp. Tier 2
  (`emitParentSpecialization`, :1218-1291) still ends by falling into
  `slowPathLab` (`return` at :974, bound at :800): the string block sits
  before the object compare and ends in unconditional jumps, so nothing new
  lies between tier 2 and `slowPathLab`.
- `objCheckLab`: reached only from that `jne`; the state is today's at
  the object compare: tag in `temp1`, the source intact, `temp2`-`temp4`
  unwritten, no live flags; the object compare reuses the tag
  (`emit_sh_ljs_tag_is_object`, :102-108) and the object path continues
  from `emit_sh_ljs_get_pointer` (:943) unchanged.
- Emit-once helpers: the object path's `Emit_sh_shv_decode` (:948) emits
  its rest cases once (the generic tier's `emitAll`, :1048, or tier 2's,
  :1278); the Method block constructs its own instance and calls
  `emitAll` once on it, so the two never share a `ptrLab`
  (JitEmitter-internal.h:540-601). `objCheckLab` is a new label bound once.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Write the failing tests**

Each file below carries, until Task 4, the lines

```js
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter emits string blocks (Task 4).
```

in place of `// REQUIRES: jit`.

`test/jit/getbyid-string-length.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter emits string blocks (Task 4).

// The Length block over every StringPrimitive kind. getLen's version 1
// is compiled before any string arrives (NotPrepared): calls 1-16
// decline, the 16th recompiles, and version 2 holds the Length block,
// which serves the eleven reads printed on the second line. lenSelf
// writes its result into its source register (BC; review focus 1); its
// version 2 serves the two reads after its warm-up.
// NumByIdStringHits = 11 + 2 = 13; NumRecompiles = 2. main() runs fewer
// than ten recording sites, once each.
// The kinds (checked once in Step 3 with a temporary print):
// - plain, ascii: concatenations shorter than 256 characters,
//   DynamicASCII (StringPrimitive.cpp:294-308);
// - utf16: String.fromCharCode with three arguments, DynamicUTF16 (one
//   argument returns a cached single-character string, String.cpp:428-435);
// - uniq: an Object.keys name, a uniqued identifier string
//   (JSObject.cpp:607-609) with LENGTH_FLAG_UNIQUED (bit 31) and its hash
//   (bit 30, IdentifierTable.cpp:351) set;
// - hashed: a flat dynamic UTF-16 string (String.fromCharCode builds
//   UTF-16, String.cpp:438) used as a computed property key, which
//   caches its hash (bit 30) on it without uniquing it
//   (IdentifierTable.cpp:101-116, :317-354);
// - prefix, buf: repeated += reaching 256 characters gives a
//   BufferedASCII string; prefix is buf before two more appends, so its
//   length (300) is less than its buffer's (302)
//   (StringPrimitive.h:749-760);
// - ext: repeat() to 65537 characters, built at the final length as
//   UTF-16 (repeat passes no ASCII flag, String.cpp:1454) and external
//   from 65536 on (String.cpp:1451-1465, StringPrimitive.h:124-125,
//   :143-144);
// - extA: btoa() of 49155 characters, 65540 ASCII characters: btoa
//   builds ASCII (Base64.cpp:42), external at that length;
// - the empty string and a one-character literal.
// Fails before the feature (no Length block; NumByIdStringHits 0), and if
// the mask lets bit 31 (uniq) or bit 30 (uniq, hashed) through.
function main() {
  function getLen(s) {
    return s.length;
  }
  function lenSelf(s) {
    s = s.length;
    return s;
  }
  function cat(a, b) {
    return a + b;
  }
  function grow(c, n) {
    var s = '';
    for (var i = 0; i < n; ++i)
      s += c;
    return s;
  }
  // n calls of f(s); the sum of the results.
  function drive(f, s, n) {
    var r = 0;
    for (var i = 0; i < n; ++i)
      r += f(s);
    return r;
  }
  var plain = cat('ab', 'cd');
  print(drive(getLen, plain, 16));
  var ascii = cat('abc', 'defg');
  var utf16 = String.fromCharCode(0x100, 0x101, 0x102);
  var uniq = Object.keys({uniqueStrLenKey: 1})[0];
  var hashed = String.fromCharCode(104, 97, 115, 104, 101, 100);
  var keyed = {};
  keyed[hashed] = 1;
  var buf = grow('x', 300);
  var prefix = buf;
  buf += 'yz';
  var ext = 'Ā'.repeat(65537);
  var extA = btoa('a'.repeat(49155));
  print(
      getLen(plain),
      getLen(ascii),
      getLen(utf16),
      getLen(uniq),
      getLen(hashed),
      getLen(prefix),
      getLen(buf),
      getLen(ext),
      getLen(extA),
      getLen(''),
      getLen('z'));
  print(drive(lenSelf, plain, 16), lenSelf(ascii), lenSelf(uniq));
}
main();

// OUT: 64
// OUT-NEXT: 4 7 3 15 6 300 302 65537 65540 0 1
// OUT-NEXT: 64 7 15
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP-NOT: JIT compilation of
// DUMP: JIT ById string sites: 1 length, 0 method
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'lenSelf' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'lenSelf' (version 2)
// DUMP-NOT: JIT compilation of
// DUMP: JIT ById string sites: 1 length, 0 method
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdStringHits: 13{{$}}
// BC-LABEL: Function<lenSelf>
// BC: GetById{{(Short)?}} [[R:r[0-9]+]], [[R]],
```

`test/jit/getbyid-string-method.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter emits string blocks (Task 4).

// Method blocks for builtins and for data properties of every value
// kind. Each function's version 1 is compiled before any string arrives
// (NotPrepared): calls 1-16 decline, call 16 recompiles, and version 2's
// Method block serves calls 17-20. 12 sites x 4 = 48 hits; 12
// recompiles.
// - cc calls the loaded charCodeAt; io (indexOf, out of line: its load
//   goes through propStorage) and sub (substring) are called too.
// - ts loads toString, a direct slot (JitStringPrepTest pins both slots;
//   review focus 2): its load has no propStorage indirection.
// - gInt ... gTrue read data properties def() adds to String.prototype
//   (out of line), holding a small integer, 1.1 (a heap BoxedDouble on
//   HV32 and BOXED: its low bits do not fit the inline encoding,
//   SmallHermesValue.h:318-336), a symbol, a heap string, an object,
//   undefined, null and true: on HV32 and BOXED every decoder branch
//   runs (x86-64/JitEmitter-internal.cpp:18-148).
// main() runs two recording reads (String.prototype, SP.toString) and
// def() eight keyed stores, both under 16.
// Fails before the feature (no Method blocks; NumByIdStringHits 0), and
// on the HV32 and BOXED trees without the block's decoder.
function main() {
  var SP = String.prototype;
  function def(o, k, v) {
    o[k] = v;
  }
  function cat(a, b) {
    return a + b;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  function cc(s) {
    return s.charCodeAt(1);
  }
  function io(s) {
    return s.indexOf('c');
  }
  function sub(s) {
    return s.substring(1, 3);
  }
  function ts(s) {
    return s.toString;
  }
  function gInt(s) {
    return s.pInt;
  }
  function gDbl(s) {
    return s.pDbl;
  }
  function gSym(s) {
    return s.pSym;
  }
  function gStr(s) {
    return s.pStr;
  }
  function gObj(s) {
    return s.pObj;
  }
  function gUndef(s) {
    return s.pUndef;
  }
  function gNull(s) {
    return s.pNull;
  }
  function gTrue(s) {
    return s.pTrue;
  }
  var sym = Symbol('ps');
  var obj = {k: 9};
  def(SP, 'pInt', 42);
  def(SP, 'pDbl', 1.1);
  def(SP, 'pSym', sym);
  def(SP, 'pStr', cat('str', 'ing'));
  def(SP, 'pObj', obj);
  def(SP, 'pUndef', undefined);
  def(SP, 'pNull', null);
  def(SP, 'pTrue', true);
  print(
      count(cc, 'abc', 20, 98),
      count(io, 'abc', 20, 2),
      count(sub, 'abcd', 20, 'bc'),
      count(ts, 'abc', 20, SP.toString));
  print(
      count(gInt, 'abc', 20, 42),
      count(gDbl, 'abc', 20, 1.1),
      count(gSym, 'abc', 20, sym),
      count(gStr, 'abc', 20, 'string'),
      count(gObj, 'abc', 20, obj),
      count(gUndef, 'abc', 20, undefined),
      count(gNull, 'abc', 20, null),
      count(gTrue, 'abc', 20, true));
}
main();

// OUT: 20 20 20 20
// OUT-NEXT: 20 20 20 20 20 20 20 20
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'cc' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'cc' (version 2)
// DUMP-NOT: JIT compilation of
// DUMP: JIT ById string sites: 0 length, 1 method
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'io' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{([6-9]|[1-9][0-9]+)}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'sub' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'ts' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-5]}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gInt' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gDbl' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gSym' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gStr' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gObj' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gUndef' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gNull' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'gTrue' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'gTrue' (version 2)
// CNT: NumRecompiles: 12{{$}}
// CNT: NumByIdStringHits: 48{{$}}
```

(Each `// Get string method` check lies between its function's
`JIT compilation of` line and the next one: if a function emitted no
block, the comment check would consume the next function's comment and
that function's `JIT compilation` check would fail.)

`test/jit/getbyid-string-mixed.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter emits string blocks (Task 4).

// A string block and a poly chain at the same site.
// getLen: 300 calls over an array, an object and a string, by i % 3.
// In version 1 every call declines: the two object classes alternate
// through the single read cache entry and strings never use it; the
// array's and the object's own `length` shapes are recorded and strings
// set sawString. Call 16 (i = 15) recompiles; version 2 emits the Length
// block, then a 2-entry poly chain, and every later call hits inline.
// Strings in version 2: i = 17, 20, ..., 299: 95 hits.
// getCC: 200 calls alternating an object with an own charCodeAt (i
// even) and a string (i odd). The object misses only at i = 0 (its class
// then stays in the read cache, which strings never touch), so the 16
// version-1 declines are i = 0 and the strings at i = 1, 3, ..., 29; call
// i = 29 recompiles. Version 2 emits the Method block, then a 1-entry
// chain. Strings in version 2: i = 31, 33, ..., 199: 85 hits.
// NumByIdStringHits = 180: object hits are not counted. NumRecompiles =
// 2; NumByIdDemotions = 0, since nothing declines in version 2.
// Fails before the feature (no string block), and if objects at a site
// with a string block missed their poly chain (NumByIdDemotions 2).
function main() {
  function getLen(x) {
    return x.length;
  }
  function getCC(x) {
    return x.charCodeAt;
  }
  // n calls of f(a), f(b), f(c) by i % 3; the sum of the results.
  function sum3(f, a, b, c, n) {
    var s = 0;
    for (var i = 0; i < n; ++i) {
      var k = i % 3;
      s += f(k === 0 ? a : k === 1 ? b : c);
    }
    return s;
  }
  // n calls alternating f(a) (i even) and f(b) (i odd);
  // "<matches of wa> <matches of wb>".
  function alt(f, a, wa, b, wb, n) {
    var ca = 0;
    var cb = 0;
    for (var i = 0; i < n; ++i) {
      if (i & 1) {
        if (f(b) === wb)
          ++cb;
      } else if (f(a) === wa) {
        ++ca;
      }
    }
    return ca + ' ' + cb;
  }
  var cc = String.prototype.charCodeAt;
  print(sum3(getLen, [1, 2], {length: 5}, 'abc', 300));
  print(alt(getCC, {charCodeAt: 7}, 7, 'abc', cc, 200));
}
main();

// OUT: 1000
// OUT-NEXT: 100 100
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: // Get polymorphic chain (2 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP-NEXT: JIT ById string sites: 1 length, 0 method
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP-NEXT: JIT ById string sites: 0 length, 1 method
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdStringHits: 180{{$}}
```

(A version-2 site that emits a poly chain is not cold, x86 :959-961, so
no `JIT cold ById sites` line precedes `JIT ById sites`.)

`test/jit/getbyid-string-replace.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter emits string blocks (Task 4).

// cc's version 2 holds a Method block for charCodeAt (calls 17-20 hit).
// Assigning a new function to String.prototype.charCodeAt changes the
// property's value, not String.prototype's class, so the block keeps
// hitting (calls 21-30); it reloads the slot on every hit, so the site
// calls the new function. NumByIdStringHits = 4 + 10 = 14; one recompile.
// Fails before the feature (no hits), and if the block returned the
// value it saw when compiled (the second count would be 0).
function main() {
  var SP = String.prototype;
  function cc(s) {
    return s.charCodeAt(0);
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(cc, 'abc', 20, 97));
  SP.charCodeAt = function () {
    return -1;
  };
  print(count(cc, 'abc', 10, -1));
}
main();

// OUT: 20
// OUT-NEXT: 10
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'cc' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMP-NOT: 'cc' (version 3)
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdStringHits: 14{{$}}
```

`test/jit/getbyid-string-gc.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter emits string blocks (Task 4).

// Method and Length blocks across collections. -gc-sanitize-handles=1
// makes a HERMESVM_SANITIZE_HANDLES build (cmake-build-x86jit-hsan) move
// every live object at every allocation and free the old copy; other
// builds ignore the flag. getLen and getCC are warmed to version 2 (16
// calls each; the 16th recompiles). Each of the 10 rounds then reads
// both through their blocks, allocates a string and collects. The
// Method block reloads runtime.stringPrototype, a root the collector
// updates (Runtime.cpp:630-643), so it follows String.prototype and its
// class to their new copies. 20 reads, all hits.
// Fails before the feature (no hits) and, on the hsan tree, if the block
// embedded String.prototype's address (an ASan report or a wrong value).
function main() {
  function getLen(s) {
    return s.length;
  }
  function getCC(s) {
    return s.charCodeAt;
  }
  function warm(f, s, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(s);
    return r;
  }
  function rounds(getLen, getCC, s, cc, collect, n) {
    var ok = 0;
    var keep = [];
    for (var i = 0; i < n; ++i) {
      if (getLen(s) === 3)
        ++ok;
      if (getCC(s) === cc)
        ++ok;
      keep[i] = 'r' + i + s;
      collect();
    }
    return ok;
  }
  var s = 'abc';
  warm(getLen, s, 16);
  warm(getCC, s, 16);
  print(rounds(getLen, getCC, s, String.prototype.charCodeAt, gc, 10));
}
main();

// CHECK: 20
// CHECK: JIT counters:
// CHECK: NumByIdStringHits: 20{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
```

(`keep[i] = ...` keeps the allocation live; its PutByVal declines are
rounds()' own, 10 at most, under 16. The first RUN line puts values and
counters on one stream, as test/jit/getbyid-record-throw.js:8 and
:46-49 do.)

- [ ] **Step 3: See them fail; verify the string kinds**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-string' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: every CNT, DUMP and gc CHECK run of the five new files FAILS
(`NumByIdStringHits: 0`, no `// Get string` comment); the OUT runs and
length.js's BC run pass. If BC fails (the compiler gave `lenSelf`
different registers), report the bytecode; review focus 1 needs a
source shape whose GetById writes its source register.

Then verify the length recipes once with a temporary print (never
committed). In `lib/VM/JIT/JitHandlers.cpp`, add
`#include "RuntimeOffsets.h"` after the other includes, and at the top
of `_jit_get_by_id`'s body:

```cpp
  if (toPHV(source)->isString()) {
    const StringPrimitive *sp = toPHV(source)->getString();
    uint32_t lf;
    memcpy(
        &lf,
        (const char *)sp + RuntimeOffsets::stringPrimitiveLengthAndFlags,
        sizeof(lf));
    llvh::errs() << "STRKIND " << cellKindStr(sp->getKind()) << " flags "
                 << (lf >> 30) << " length " << sp->getStringLength()
                 << "\n";
  }
```

```bash
cmake --build cmake-build-x86jit --target hermes
cmake-build-x86jit/bin/hermes -fno-inline -Xjit=force -Xjit-max-recompiles=0 \
  test/jit/getbyid-string-length.js 2>&1 | grep STRKIND | sed -n 19,29p
```

(`-Xjit-max-recompiles=0` keeps every call in version 1, so every read
reaches the helper. Lines 1-16 are the warm-up, 17-18 main's own
`repeat` reads, 19-29 the eleven reads of the second print line.)
Expected, in order: DynamicASCII flags 0 length 4; DynamicASCII 0 7;
DynamicUTF16 0 3; a uniqued kind (DynamicUniquedASCII expected) with
flags 2 or 3 (bit 31 set), length 15; DynamicUTF16 flags 1 length 6;
BufferedASCII 0 300; BufferedASCII 0 302; ExternalUTF16 0 65537;
ExternalASCII 0 65540; then the empty and one-character literals. Add
`#include "hermes/VM/CellKind.h"` and `<cstring>` temporarily if the
print does not compile. If a recipe gives another kind or flags, change
the recipe (not the expectation) and say so in the test's comment.
Remove the print and the include (`git diff lib/VM/JIT/JitHandlers.cpp`
must be empty) and rebuild.

- [ ] **Step 4: The planning and the object check**

In `lib/VM/JIT/x86-64/JitEmitter-property.cpp`, `emitFastPath`, replace
lines 918-941 (from `// Get register assignments for the rest of the
temporaries.` through the `a.jne(slowPathLab);` after the object check)
with:

```cpp
    // Get register assignments for the rest of the temporaries.
    HWReg hwTemp2 = _.allocTempGpX();
    HWReg hwTemp3 = _.allocTempGpX();
    HWReg hwTemp4 = _.allocTempGpX();
    temp2 = hwTemp2.gpq();
    temp3 = hwTemp3.gpq();
    temp4 = hwTemp4.gpq();
    // The Length block converts in a vector temporary. Allocating it
    // emits no code: after the frees above no vector temp holds an FR.
    HWReg hwStrVTmp = strTier_ == JitByIdStringTier::Length
        ? _.allocTempVecD()
        : HWReg{};

    // Now that we have recorded their registers, mark all temp registers as
    // free.
    _.freeReg(hwTemp1);
    _.freeReg(hwTemp2);
    _.freeReg(hwTemp3);
    _.freeReg(hwTemp4);
    if (hwStrVTmp.isValid())
      _.freeReg(hwStrVTmp);

    // Allocate the result register. Note that it can overlap the temps we
    // just freed.
    hwRes = _.getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));

    // Finally we begin code generation for the fast path.

    if (strTier_ == JitByIdStringTier::Length ||
        strTier_ == JitByIdStringTier::Method) {
      // String receivers first (2026-10-26 string spec, "Emission"): one
      // tag extraction serves both compares, and the block never falls
      // through.
      asmjit::Label objCheckLab = a.newLabel();
      emit_sh_ljs_is_string(a, temp1, hwSourceGpx.gpq());
      a.jne(objCheckLab);
      emitStringBlock(hwSourceGpx.gpq(), hwStrVTmp);
      a.bind(objCheckLab);
      // As at the plain object check: the tag in temp1, the source
      // register intact, temp2-temp4 unwritten.
      emit_sh_ljs_tag_is_object(a, temp1);
      a.jne(slowPathLab);
    } else {
      // Is the input an object.
      emit_sh_ljs_is_object(a, temp1, hwSourceGpx.gpq());
      a.jne(slowPathLab);
    }
```

The lines after it (`// temp1 is the pointer to the object.` on) are
unchanged.

- [ ] **Step 5: The string block**

After `emitFastPath`'s closing brace (line 1050), before the doc comment
of `emitPolyChain`, add:

```cpp

  /// Emit the string block (2026-10-26 string spec, "Emission") for a
  /// string receiver whose tag is in temp1. The Length block reads \p src
  /// only in its first instruction; the Method block never reads it. Both
  /// write hwRes only on a hit, as the last write before `jmp contLab`. A
  /// Method miss jumps to slowPathLab having written only temp2, temp3
  /// and EFLAGS. Never falls through.
  /// \param hwVTmp the Length block's vector temporary; invalid for Method.
  void emitStringBlock(const x86::Gp &src, HWReg hwVTmp) {
    if (strTier_ == JitByIdStringTier::Length) {
      _.comment("// Get string length (site %u)", siteId_);
      // temp1 = lengthAndFlags; the 32-bit load zero-extends.
      emit_stringprim_get_length_and_flags(a, temp1, src);
      a.and_(
          temp1.r32(), asmjit::Imm(RuntimeOffsets::stringPrimitiveLengthMask));
      // Uses r11 and EFLAGS, neither live here.
      _.emitIncrementCounter(JitCounter::NumByIdStringHits);
      // Exact: the length is below 2^30 and temp1's upper half is zero.
      a.vcvtsi2sd(hwVTmp.xmm(), hwVTmp.xmm(), temp1);
      // A number HermesValue is the double's bit pattern in every heap
      // mode (see emitGetByValTypedArrayTier).
      a.vmovq(hwRes.gpq(), hwVTmp.xmm());
      a.jmp(contLab);
      return;
    }
    assert(
        strTier_ == JitByIdStringTier::Method &&
        "a string block needs a Length or Method tier");
    _.comment(
        "// Get string method (site %u, slot %u)",
        siteId_,
        (unsigned)strSlot_);
    // temp2 = String.prototype, reloaded on every execution: root marking
    // updates runtime.stringPrototype when the object moves.
    a.mov(temp2, x86::qword_ptr(xRuntime, RuntimeOffsets::stringPrototype));
    emit_sh_ljs_get_pointer(a, temp2, temp2);
    // temp3 = its class's lazy JIT ID.
    emit_load_cp(a, temp3, x86::ptr(temp2, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, temp3);
    a.movzx(
        temp3.r32(),
        x86::word_ptr(temp3, RuntimeOffsets::hiddenClassLazyJITId));
    a.cmp(temp3.r32(), asmjit::Imm(site_->stringProtoId));
    a.jne(slowPathLab);
    // Uses r11 and EFLAGS, neither live here.
    _.emitIncrementCounter(JitCounter::NumByIdStringHits);
    // May rewrite temp2 (out-of-line slot); hwRes may be temp2.
    emitLoadFromSlot(hwRes.gpq(), temp2, strSlot_);
    // Its own decoder: the object path's rest cases are emitted once, by
    // the object path.
    Emit_sh_shv_decode strDecode(a, hwRes.gpq(), contLab);
    strDecode.emitAll(a);
    a.jmp(contLab);
  }
```

- [ ] **Step 6: Run the new tests**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-string' cmake --build cmake-build-x86jit --target check-hermes
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed cmake-build-x86jit-hsan; do
  cmake --build $t --target hermes &&
  LIT_FILTER='getbyid-string' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: all PASS on all four trees; on the hsan tree every new file
RUNS (none may be reported unsupported). Every expected count is derived
in its test's comment; if a count differs, find the cause (a
`-Xdump-jitcode=3` run shows which versions compiled and which blocks
they hold) and fix the code or the derivation, never just the number.

- [ ] **Step 7: Prove they can fail**

One at a time on cmake-build-x86jit unless stated; rebuild after each
change and each restore:
- Mutation A: in `byIdStringCanProgress`, `return false;` for
  NotPrepared → length.js and method.js FAIL (no version 2,
  `NumByIdStringHits: 0`).
- Mutation B: in the Length block, mask with
  `RuntimeOffsets::stringPrimitiveLengthMask | 0x80000000u` →
  length.js OUT FAILS (uniq's length).
- Mutation C: mask with `RuntimeOffsets::stringPrimitiveLengthMask |
  0x40000000u` → length.js OUT FAILS (uniq's and hashed's lengths).
- Mutation D: delete `strDecode.emitAll(a);` → rebuild
  cmake-build-x86jit-hv32 and cmake-build-x86jit-boxed and run
  method.js on each: it FAILS on both (values returned undecoded). On HV64
  the decoder emits nothing, so cmake-build-x86jit passes; say so.
- Mutation E: at `objCheckLab`, compare with `emit_sh_ljs_tag_is_string`
  instead of `emit_sh_ljs_tag_is_object` → mixed.js FAILS
  (`NumByIdDemotions: 2`: objects always miss).
- Mutation F: replace the `a.mov(temp2, x86::qword_ptr(xRuntime, ...))`
  load with `_.loadBits64InGp(temp2,
  _.runtime_.stringPrototype.getHermesValue().getRaw(),
  "String.prototype")` → on cmake-build-x86jit-hsan, gc.js FAILS (an
  ASan report or a wrong count). If it passes there, the block did not
  see a moved String.prototype: report it before continuing.
- Mutation G: replace `emitLoadFromSlot(hwRes.gpq(), temp2, strSlot_);`
  with `_.loadBits64InGp(hwRes.gpq(),
  JSObject::getNamedSlotValueUnsafe(vmcast<JSObject>(
  _.runtime_.stringPrototype.getHermesValue()), _.runtime_,
  strSlot_).unboxToHV(_.runtime_).getRaw(), "method")` (and skip the
  decoder) → replace.js FAILS (second count 0).

- [ ] **Step 8: Suites**

```bash
cmake --build cmake-build-x86jit --target check-hermes
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed cmake-build-x86jit-hsan; do
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes &&
  LIT_FILTER='jit/' cmake --build cmake-build-arm64 --target check-hermes ||
  echo "FAIL arm64"
```

Expected: no failure (on the hsan tree, at most the known flake). On
arm64 the five new files are unsupported. An existing x86 test whose
string GetById now hits inline may decline less (fewer crossings, fewer
demotions): reconcile as in Task 2 Step 11.

- [ ] **Step 9: Commit**

```bash
git add lib/VM/JIT/x86-64/JitEmitter-property.cpp \
  test/jit/getbyid-string-length.js test/jit/getbyid-string-method.js \
  test/jit/getbyid-string-mixed.js test/jit/getbyid-string-replace.js \
  test/jit/getbyid-string-gc.js
git commit   # "JIT: inline GetById on string receivers on x86-64"
```

(Add any other test file Step 8 changed, by path.)

---

### Task 4: arm64 string blocks

**Files:**
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp:1026-1049`
  (`emitFastPath` planning and object check), new `emitStringBlock`
  after `emitFastPath` (before line 1154)
- Modify: the five Task 3 tests (drop the x86-only lines)
- Test: `test/jit/getbyid-string-bigid.js`

**Interfaces:**
- Consumes: Tasks 1-3; arm64 `emit_sh_ljs_is_string`
  (arm64/JitEmitter-internal.h:164-170), `emit_sh_ljs_tag_is_object`
  (:58-64), `emit_sh_ljs_get_pointer` (:78-88, an `and`, which leaves the
  input intact), `emit_stringprim_get_length_and_flags`
  (JitEmitter-internal.cpp:276-283), `emit_load_from_base_offset<8>`
  (JitEmitter-internal.h:352-412: one `ldr` below 32768, else
  `mov tmp, ofs/8; ldr dest, [base, tmp, lsl 3]` with `tmp != base`),
  `emit_cmp_imm32` (:572-583: `mov wTmp, imm` when the ID is not an
  add/sub immediate), `emitLoadFromSlot(res, obj, tmp, slot)`
  (arm64 JitEmitter-property.cpp:965-1000, `tmp != obj`, `tmp` may be
  `res`), `emitIncrementCounter` (arm64 JitEmitter.cpp:656-670: saves and
  restores x0/x1 itself, no flags).
- Produces: arm64 `void emitStringBlock(const a64::GpX &xSrc, HWReg
  hwVTmp)`.

Register contract: as Task 3's, with `xTemp1`-`xTemp4` (planning at
:1006-1043, object check at :1047-1051, shvDecode at :1056, tier 2 at
:1076-1083 and :1313-1378). Additionally `x16` (xScratch,
arm64/JitEmitter.h:208) is written by the runtime-field load only if the
offset is 32768 or more, and by the ID compare only for IDs that are not
add/sub immediates; both writes precede the guard's `b.ne`, and the slow
call overwrites `x16` itself (`callRuntimeWithSavedIPIndirect`, :951-952),
so it is dead at `slowPathLab`. `xTemp4` is `emitLoadFromSlot`'s
large-offset temporary, used only on the hit path. The arm64
`Emit_sh_shv_decode` needs no scratch register.

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Lift the gates; write the bigid test; see them fail**

In each of getbyid-string-length.js, -method.js, -mixed.js,
-replace.js and -gc.js replace the two lines
`// REQUIRES: jit, jit-arch-x86-64` and
`// x86-64 only until the arm64 emitter emits string blocks (Task 4).`
with `// REQUIRES: jit`.

`test/jit/getbyid-string-bigid.js` (review focus 3):

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=2 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=DUMP,DUMP%jit_arch_prefix %s
// REQUIRES: jit

// String.prototype's class gets a lazy JIT ID above 4095 that is not a
// multiple of 4096, which arm64's compare cannot take as an immediate
// (a64::Utils::isAddSubImm), so the Method block's compare materializes
// it in w16. As in getbyid-poly-emitted-arm64.js, 4200 tiny functions
// compiled warm first each assign one ID. getCC has no loop, so its
// first two calls run in the interpreter (it compiles once its execution
// count reaches 2, include/hermes/VM/JIT/x86-64/JIT.h:289-292); version 1
// (no string seen by JIT code: NotPrepared) declines on calls 3-18 and
// call 18 recompiles; calls 19-100 hit: 82. An unrelated property then
// gives String.prototype a class without an ID: the next 8 calls must
// miss (8 declines, no crossing), so NumByIdStringHits stays 82.
// Fails before the feature (0 hits) and, on arm64, if the compare lost
// the ID (a compare against itself always matches: 90 hits).
function main() {
  var N = 4200;
  for (var i = 0; i < N; ++i) {
    var o = {};
    o['k' + i] = i;
    o.x = 1;
    var f = new Function('o', 'return o.x;');
    // Two interpreted calls fill the cache; the third compiles and
    // specializes on o's class, assigning it an ID.
    f(o);
    f(o);
    f(o);
  }
  var SP = String.prototype;
  function getCC(s) {
    return s.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  var cc = SP.charCodeAt;
  print(count(getCC, 'abc', 100, cc));
  SP.unrelatedBig = 1;
  print(count(getCC, 'abc', 8, cc));
}
main();

// OUT: 100
// OUT-NEXT: 8
// CNT: NumByIdStringHits: 82{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// DUMPX86: movzx {{.*}}word ptr
// DUMPARM64: ldrh w{{[0-9]+}}
// DUMPARM64: mov w16, 0x{{[0-9A-F]+}}
// DUMPARM64-NEXT: cmp w{{[0-9]+}}, w16
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getCC' (version 2)
```

(`main` has a loop, so it compiles at once and its PutById/PutByVal
misses recompile it within its budget; this test therefore pins no
NumRecompiles. `%jit_arch_prefix` is DUMPX86 or DUMPARM64, as in
getbyid-poly-rooted-str.js:9 and :47-48.)

```bash
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
LIT_FILTER='getbyid-string' cmake --build cmake-build-arm64 --target check-hermes
```

Expected: on arm64 the CNT, DUMP and gc CHECK runs of the six files
FAIL (no blocks, `NumByIdStringHits: 0`); absent.js and hcid.js pass.

- [ ] **Step 3: The arm64 planning and object check**

In `lib/VM/JIT/arm64/JitEmitter-property.cpp`, `emitFastPath`, replace
lines 1026-1049 (from `// Get register assignments for the rest of the
temporaries.` through the `a.b_ne(slowPathLab);` after the object check)
with:

```cpp
    // Get register assignments for the rest of the temporaries.
    HWReg hwTemp2Gpx = _.allocTempGpX();
    HWReg hwTemp3Gpx = _.allocTempGpX();
    HWReg hwTemp4Gpx = _.allocTempGpX();
    xTemp2 = hwTemp2Gpx.a64GpX();
    xTemp3 = hwTemp3Gpx.a64GpX();
    xTemp4 = hwTemp4Gpx.a64GpX();
    // The Length block converts in a vector temporary. Allocating it
    // emits no code: after the frees above no vector temp holds an FR.
    HWReg hwStrVTmp = strTier_ == JitByIdStringTier::Length
        ? _.allocTempVecD()
        : HWReg{};

    // Now that we have recorded their registers, mark all temp registers as
    // free.
    _.freeReg(hwTemp1Gpx);
    _.freeReg(hwTemp2Gpx);
    _.freeReg(hwTemp3Gpx);
    _.freeReg(hwTemp4Gpx);
    if (hwStrVTmp.isValid())
      _.freeReg(hwStrVTmp);

    // Allocate the result register. Note that it can overlap the temps we
    // just freed.
    hwRes = _.getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));

    // Finally we begin code generation for the fast path.

    if (strTier_ == JitByIdStringTier::Length ||
        strTier_ == JitByIdStringTier::Method) {
      // String receivers first (2026-10-26 string spec, "Emission"): one
      // tag extraction serves both compares, and the block never falls
      // through.
      asmjit::Label objCheckLab = a.newLabel();
      emit_sh_ljs_is_string(a, xTemp1, hwSourceGpx.a64GpX());
      a.b_ne(objCheckLab);
      emitStringBlock(hwSourceGpx.a64GpX(), hwStrVTmp);
      a.bind(objCheckLab);
      // As at the plain object check: the tag in xTemp1, the source
      // register intact, xTemp2-xTemp4 unwritten.
      emit_sh_ljs_tag_is_object(a, xTemp1);
      a.b_ne(slowPathLab);
    } else {
      // Is the input an object.
      emit_sh_ljs_is_object(a, xTemp1, hwSourceGpx.a64GpX());
      a.b_ne(slowPathLab);
    }
```

- [ ] **Step 4: The arm64 string block**

After `emitFastPath`'s closing brace (line 1152), before the doc comment
of `emitPolyChain`, add:

```cpp

  /// Emit the string block (2026-10-26 string spec, "Emission") for a
  /// string receiver whose tag is in xTemp1. The Length block reads
  /// \p xSrc only in its first instruction; the Method block never reads
  /// it. Both write hwRes only on a hit, as the last write before
  /// `b contLab`. A Method miss branches to slowPathLab having written
  /// only xTemp2, xTemp3, x16 and NZCV. Never falls through.
  /// \param hwVTmp the Length block's vector temporary; invalid for Method.
  void emitStringBlock(const a64::GpX &xSrc, HWReg hwVTmp) {
    if (strTier_ == JitByIdStringTier::Length) {
      _.comment("// Get string length (site %u)", siteId_);
      // xTemp1 = lengthAndFlags; the 32-bit load zero-extends.
      emit_stringprim_get_length_and_flags(a, xTemp1, xSrc);
      a.and_(xTemp1, xTemp1, RuntimeOffsets::stringPrimitiveLengthMask);
      // Saves and restores x0/x1 itself; writes no flags.
      _.emitIncrementCounter(JitCounter::NumByIdStringHits);
      // Exact: the length is below 2^30.
      a.ucvtf(hwVTmp.a64VecD(), xTemp1.w());
      // A number HermesValue is the double's bit pattern in every heap
      // mode (see emitGetByValTypedArrayTier).
      a.fmov(hwRes.a64GpX(), hwVTmp.a64VecD());
      a.b(contLab);
      return;
    }
    assert(
        strTier_ == JitByIdStringTier::Method &&
        "a string block needs a Length or Method tier");
    _.comment(
        "// Get string method (site %u, slot %u)",
        siteId_,
        (unsigned)strSlot_);
    // xTemp2 = String.prototype, reloaded on every execution: root marking
    // updates runtime.stringPrototype when the object moves. x16 is the
    // temporary for an offset of 32768 or more.
    emit_load_from_base_offset<8>(
        a, xTemp2, xRuntime, xScratch, RuntimeOffsets::stringPrototype);
    emit_sh_ljs_get_pointer(a, xTemp2, xTemp2);
    // xTemp3 = its class's lazy JIT ID.
    emit_load_cp(a, xTemp3, a64::Mem(xTemp2, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, xTemp3);
    a.ldrh(xTemp3.w(), a64::Mem(xTemp3, RuntimeOffsets::hiddenClassLazyJITId));
    emit_cmp_imm32(a, xTemp3.w(), site_->stringProtoId, xScratch.w());
    a.b_ne(slowPathLab);
    _.emitIncrementCounter(JitCounter::NumByIdStringHits);
    // May rewrite xTemp2 (out-of-line slot) and xTemp4 (large offset);
    // hwRes may be either.
    emitLoadFromSlot(hwRes.a64GpX(), xTemp2, xTemp4, strSlot_);
    // Its own decoder: the object path's rest cases are emitted once, by
    // the object path.
    Emit_sh_shv_decode strDecode(a, hwRes.a64GpX(), contLab);
    strDecode.emitAll(a);
    a.b(contLab);
  }
```

- [ ] **Step 5: Build all three arm64 trees and run**

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  ./aarch64/qemu-sanity.sh $t &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no `FAIL`; every getbyid-string test passes on every arm64
tree with the counts of x86-64 (the tests pin only comments, counters,
summary lines and output, which are backend neutral, plus bigid.js's
DUMPARM64 lines). An arm64-only failure is a finding about the port:
debug it (superpowers:systematic-debugging) and fix the arm64 emitter;
never gate the test.

- [ ] **Step 6: Prove the arm64 blocks are pinned**

On cmake-build-arm64 unless stated (rebuild host first; restore and
rebuild after each):
- Mutation A: mask with `0x7fffffffu` (bit 30 let through; a mask with
  bit 31 but not bit 30 is not an encodable logical immediate) in the
  arm64 Length block → length.js FAILS (uniq's and hashed's lengths).
- Mutation B: delete `strDecode.emitAll(a);` → on cmake-build-arm64-hv32
  and cmake-build-arm64-boxed, method.js FAILS.
- Mutation C: pass `xTemp3.w()` as `emit_cmp_imm32`'s temporary →
  bigid.js FAILS (`NumByIdStringHits: 90` and no `w16` compare).
- Mutation D: at `objCheckLab`, compare with `emit_sh_ljs_tag_is_string`
  → mixed.js FAILS (`NumByIdDemotions: 2`).

- [ ] **Step 7: x86 still green**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: pass, including bigid.js (`git diff --stat HEAD` shows only
the arm64 emitter and the six test files).

- [ ] **Step 8: Commit**

```bash
git add lib/VM/JIT/arm64/JitEmitter-property.cpp \
  test/jit/getbyid-string-length.js test/jit/getbyid-string-method.js \
  test/jit/getbyid-string-mixed.js test/jit/getbyid-string-replace.js \
  test/jit/getbyid-string-gc.js test/jit/getbyid-string-bigid.js
git commit   # "JIT: inline GetById on string receivers on arm64"
```

---

### Task 5: Invalidation, demotion and other receivers

Tests of the class guard, the progress terms and the receivers the block
must not take, on both backends. A fix goes in this task if a test
exposes a bug (debug with superpowers:systematic-debugging; the test
becomes the regression gate).

**Files:**
- Test: `test/jit/getbyid-string-redefine.js`,
  `getbyid-string-class-change.js`, `getbyid-string-dict-add.js`,
  `getbyid-string-dict.js`, `getbyid-string-demoted.js`,
  `getbyid-string-other-prim.js`

**Interfaces:**
- Consumes: Tasks 1-4.

Facts the tests rely on, beyond the spec's "Class guards": deleting from
a non-dictionary String.prototype gives a new cacheable dictionary, and
from a cacheable one a new no-cache dictionary
(HiddenClass.cpp:432-434); a dictionary adds in place (:455-482) and
reuses the most recently freed slot (DictPropertyMap.cpp:233-255); a
primitive's GetById reads `String.prototype` with the primitive as the
receiver (Interpreter-slowpaths.cpp:1584-1610), so a getter sees a
primitive `this` in strict code; an accessor read never fills the read
cache (JSObject.cpp:1195-1228), so calls on an object with an own
accessor always decline and record nothing.

- [ ] **Step 1: Write the tests**

`test/jit/getbyid-string-redefine.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- a | %FileCheck --check-prefix=OUTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- b | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- c | %FileCheck --check-prefix=OUTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- d | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- a 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- b 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- c 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- d 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// Mutations of a hit Method block's property, one scenario per process
// (String.prototype cannot be reset). In each, getFoo's version 2 is
// compiled at its 16th call against String.prototype's current class
// and its Method block serves calls 17-20 (4 hits, the positive
// evidence); then the property changes and the next reads, before any
// recompile (two declines at most), must see the change.
// (a) foo redefined as an accessor with a strict getter: a flags
//     transition, a new class with no ID (HiddenClass.cpp:613 on); the
//     read runs the getter with a primitive `this`: "string".
// (b) delete foo (String.prototype becomes a cacheable dictionary, a new
//     class), read while absent: undefined; then bar takes foo's freed
//     slot and foo returns in a new slot: 3.
// (c) and (d): (a) and (b) from a cacheable-dictionary String.prototype
//     (an added property deleted before anything compiles); the
//     redefinition and the deletion each give a new no-cache dictionary
//     (HiddenClass.cpp:432-434, :595-611).
// Fails before the feature (no hits) and if the class-ID guard is not
// checked: (b) and (d) then read bar's 2 from foo's old slot, and (a) and
// (c) return the slot's accessor pair instead of the getter's result.
function main(scenario) {
  var SP = String.prototype;
  function getFoo(s) {
    return s.foo;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  if (scenario === 'c' || scenario === 'd') {
    SP.tmpDict = 0;
    delete SP.tmpDict;
  }
  SP.foo = 1;
  print(count(getFoo, 'abc', 20, 1));
  if (scenario === 'a' || scenario === 'c') {
    Object.defineProperty(SP, 'foo', {
      get: function () {
        'use strict';
        return typeof this;
      },
      configurable: true,
    });
    print(getFoo('abc'));
  } else {
    delete SP.foo;
    var absent = getFoo('abc');
    SP.bar = 2;
    SP.foo = 3;
    print(absent, getFoo('abc'));
  }
}
main(hermescli.getScriptArgs()[0]);

// OUTA: 20
// OUTA-NEXT: string
// OUTB: 20
// OUTB-NEXT: undefined 3
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdStringHits: 4{{$}}
```

`test/jit/getbyid-string-class-change.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// getCC's version 2 holds a Method block guarded by String.prototype's
// class ID; calls 17-20 hit (4). An unrelated property moves
// String.prototype to a new class with no lazy ID, so the guard misses
// and version 2 declines again. At its 16th decline (the 16th call of
// the second count) the class-ID term reads 0 against the stored ID and
// recompiles; version 3 prepares against the new class, and calls 17-40
// of the second count hit (24). NumByIdStringHits = 28, NumRecompiles =
// 2, NumByIdDemotions = 0.
// Fails before the feature, and without the class-ID progress term (no
// version 3: 4 hits, NumRecompiles 1).
function main() {
  var SP = String.prototype;
  function getCC(s) {
    return s.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  var cc = SP.charCodeAt;
  print(count(getCC, 'abc', 20, cc));
  SP.unrelatedCC = 1;
  print(count(getCC, 'abc', 40, cc));
}
main();

// OUT: 20
// OUT-NEXT: 40
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 3)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdStringHits: 28{{$}}
```

`test/jit/getbyid-string-dict-add.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// String.prototype is a cacheable dictionary (an added property
// deleted), whose class keeps its identity and lazy ID when a property is
// added. getFoo's version 2 (call 16, NotPrepared) prepares `foo` Absent
// with the class's property count; calls 17-24 decline. Adding foo raises
// the count without changing the class, so at version 2's 16th decline
// (call 32, the 8th of the second count) only the count term can
// progress, and it recompiles; version 3 finds foo and emits a Method
// block: calls 33-64 hit (32). NumRecompiles = 2, NumByIdDemotions = 0,
// NumByIdStringHits = 32.
// Fails before the feature, and without the count term (no version 3:
// version 2 demotes at call 64; NumRecompiles 1, NumByIdDemotions 1, 0
// hits).
function main() {
  var SP = String.prototype;
  SP.tmpDict = 0;
  delete SP.tmpDict;
  function getFoo(s) {
    return s.foo;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(getFoo, 'abc', 24, undefined));
  SP.foo = 42;
  print(count(getFoo, 'abc', 40, 42));
}
main();

// OUT: 24
// OUT-NEXT: 40
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getFoo' (version 3)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdStringHits: 32{{$}}
```

`test/jit/getbyid-string-dict.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// String.prototype is a cacheable dictionary before anything compiles. A
// cacheable dictionary is eligible (JitHandlers.cpp:542-546), so getCC's
// version 2 (call 16) emits a Method block; calls 17-40 hit (24).
// Fails before the feature, and if dictionaries were Ineligible (0 hits).
function main() {
  var SP = String.prototype;
  SP.tmpDict = 0;
  delete SP.tmpDict;
  function getCC(s) {
    return s.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  print(count(getCC, 'abc', 40, SP.charCodeAt));
}
main();

// OUT: 40
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getCC' (version 2)
// DUMP: // Get string method (site {{[0-9]+}}, slot {{[0-9]+}})
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdStringHits: 24{{$}}
```

`test/jit/getbyid-string-demoted.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- a | %FileCheck --check-prefix=OUTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- b | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- c | %FileCheck --check-prefix=OUTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods %s -- d | %FileCheck --check-prefix=OUTD %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- a 2>&1 | %FileCheck --check-prefix=CNTA %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- b 2>&1 | %FileCheck --check-prefix=CNTB %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- c 2>&1 | %FileCheck --check-prefix=CNTC %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xhermes-internal-test-methods -Xjit-emit-counters %s -- d 2>&1 | %FileCheck --check-prefix=CNTD %s
// REQUIRES: jit

// Demoted sites and string receivers (spec: "Progress", after the table),
// one scenario per process. f(o, q) holds two sites: S (`o.foo`, run when
// q is null; String.prototype.foo is 'F') and T (`q.bar` on accT, an own
// accessor: never recorded, every call declines). T never runs before
// phase 3, so it is still recording then (an unseen site is never
// demoted, JitCompiler.cpp:457-458), and its first crossing is a
// crossing of the same body. acc is an own accessor `foo` ('G'): S
// declines on it forever. Phases: 1 demotes S; 2 reads 8 strings at S; 3
// runs T 16 times (one crossing; T's own first crossing changes, nothing
// of T's progresses); 4 reads 8 strings at S.
// (a) Phase 1: 64 acc calls. Crossings at calls 16 (S's first seen:
//     changed), 32, 48 and 64; nothing progresses; the fourth demotes S
//     to the plain helper, which records nothing, so strings never set
//     sawString: phase 3's crossing has nothing to do. Hits 0,
//     NumRecompiles 0, NumByIdDemotions 1. A regression pin: it passes
//     before the feature too, and no mutation of this plan's code reaches
//     it.
// (b) Phase 1: 112 calls alternating `chained` (foo two prototypes up:
//     a chain entry) and acc. Call 16 recompiles (the unemitted chain
//     entry); version 2's chain entry hits on the 48 chained calls 17-111
//     (odd calls, i even); its 48 acc calls cross at calls 48, 80 and 112
//     with nothing to learn, and the third demotes S to
//     _jit_get_by_id_refresh (an emitted chain entry), which records
//     nothing. Hits 0, NumRecompiles 1, NumByIdDemotions 1,
//     NumByIdChainHits 48. Fails if the refresh helper recorded strings
//     (phase 3's crossing would recompile on NotPrepared; phase 4 would
//     hit 8).
// (c) Phase 1: 112 calls alternating 'abc' and acc. Call 16 recompiles
//     (NotPrepared); version 2's Method block hits the 48 strings of
//     calls 17-112, its 48 acc calls demote S at call 112 (crossings at
//     48, 80, 112). An unrelated property then changes String.prototype's
//     class: phase 2's strings miss into the plain helper (no declines).
//     Phase 3's crossing finds S's class-ID term (0 against the stored ID)
//     and recompiles; version 3 prepares S again (its slot stays
//     demoted), so phase 4's 8 strings hit. Hits 56, NumRecompiles 2,
//     NumByIdDemotions 1. Fails without the class-ID term (48 hits).
// (d) As (c) without phase 3: no crossing, so no recompile; phases 2 and
//     4 read through the plain helper. Hits 48, NumRecompiles 1. Fails if
//     the guard is not checked (64 hits: foo keeps its slot, so the
//     values alone cannot tell).
function main(scenario) {
  var SP = String.prototype;
  SP.foo = 'F';
  function f(o, q) {
    return q ? q.bar : o.foo;
  }
  // n calls of f(x, q); how many returned want.
  function count(f, x, q, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x, q) === want)
        ++c;
    return c;
  }
  // n calls alternating f(a, null) (i even) and f(b, null) (i odd);
  // "<matches of wa> <matches of wb>".
  function alt(f, a, wa, b, wb, n) {
    var ca = 0;
    var cb = 0;
    for (var i = 0; i < n; ++i) {
      if (i & 1) {
        if (f(b, null) === wb)
          ++cb;
      } else if (f(a, null) === wa) {
        ++ca;
      }
    }
    return ca + ' ' + cb;
  }
  var acc = {
    get foo() {
      return 'G';
    },
  };
  var accT = {
    get bar() {
      return 'B';
    },
  };
  if (scenario === 'a') {
    print(count(f, acc, null, 64, 'G'));
  } else if (scenario === 'b') {
    var chained = Object.create(Object.create({foo: 'C'}));
    print(alt(f, chained, 'C', acc, 'G', 112));
  } else {
    print(alt(f, 'abc', 'F', acc, 'G', 112));
  }
  if (scenario === 'c' || scenario === 'd')
    SP.unrelated = 1;
  print(count(f, 'abc', null, 8, 'F'));
  if (scenario !== 'd')
    print(count(f, 'abc', accT, 16, 'B'));
  print(count(f, 'abc', null, 8, 'F'));
}
main(hermescli.getScriptArgs()[0]);

// OUTA: 64
// OUTA-NEXT: 8
// OUTA-NEXT: 16
// OUTA-NEXT: 8
// OUTB: 56 56
// OUTB-NEXT: 8
// OUTB-NEXT: 16
// OUTB-NEXT: 8
// OUTD: 56 56
// OUTD-NEXT: 8
// OUTD-NEXT: 8
// CNTA: NumRecompiles: 0{{$}}
// CNTA: NumByIdDemotions: 1{{$}}
// CNTA: NumByIdStringHits: 0{{$}}
// CNTB: NumRecompiles: 1{{$}}
// CNTB: NumByIdDemotions: 1{{$}}
// CNTB: NumByIdChainHits: 48{{$}}
// CNTB: NumByIdStringHits: 0{{$}}
// CNTC: NumRecompiles: 2{{$}}
// CNTC: NumByIdDemotions: 1{{$}}
// CNTC: NumByIdStringHits: 56{{$}}
// CNTD: NumRecompiles: 1{{$}}
// CNTD: NumByIdDemotions: 1{{$}}
// CNTD: NumByIdStringHits: 48{{$}}
```

(Call numbering in the comment is 1-based; `alt`'s i is 0-based, so the
1-based odd calls are its even i. In (a), "nothing progresses": S has no
entries and no sawString; T has not run. In (b), S's version 2 is not
cold (it emitted a chain), and T's cache is never filled, so no cold-site
term fires either.)

`test/jit/getbyid-string-other-prim.js` (review focus 4 and 5):

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit

// Receivers that must not take a string block, at sites that hold one.
// getLen's and getCC's version 2 (call 16 each) hold the Length and
// Method blocks; calls 17-20 hit (8). Then numbers and booleans (not
// pointers), symbols (not a pointer tag), BigInts (the pointer tag next
// to the string tag) and String objects (object tag) read through the
// object path or the slow call: 5 declines per site, under 16. null and
// undefined throw TypeError from the slow call, which longjmps past the
// helper (no decline). The last two counts hit again: 20.
// NumByIdStringHits = 28, NumRecompiles = 2.
// Fails before the feature (no hits), and if the string compare admitted
// other pointer tags (the BigInt and the String object would be read as
// StringPrimitives).
function main() {
  var SP = String.prototype;
  function getLen(x) {
    return x.length;
  }
  function getCC(x) {
    return x.charCodeAt;
  }
  // n calls of f(x); how many returned want.
  function count(f, x, n, want) {
    var c = 0;
    for (var i = 0; i < n; ++i)
      if (f(x) === want)
        ++c;
    return c;
  }
  // "TypeError" if f(x) throws one.
  function tryGet(f, x) {
    try {
      f(x);
      return 'no throw';
    } catch (e) {
      return e instanceof TypeError ? 'TypeError' : 'other';
    }
  }
  var cc = SP.charCodeAt;
  print(count(getLen, 'abc', 20, 3), count(getCC, 'abc', 20, cc));
  print(
      getLen(5),
      getLen(true),
      getLen(new String('abcde')),
      getLen(Symbol('q')),
      getLen(10n));
  print(
      getCC(5),
      getCC(false),
      getCC(new String('x')) === cc,
      getCC(Symbol('q')),
      getCC(10n));
  print(tryGet(getLen, null), tryGet(getLen, undefined), tryGet(getCC, null));
  print(count(getLen, 'abcd', 10, 4), count(getCC, 'xy', 10, cc));
}
main();

// OUT: 20 20
// OUT-NEXT: undefined undefined 5 undefined undefined
// OUT-NEXT: undefined undefined true undefined undefined
// OUT-NEXT: TypeError TypeError TypeError
// OUT-NEXT: 10 10
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdStringHits: 28{{$}}
```

- [ ] **Step 2: Run them on x86**

```bash
cmake --build cmake-build-x86jit --target hermes
LIT_FILTER='getbyid-string' cmake --build cmake-build-x86jit --target check-hermes
```

Expected: all PASS. A count that differs from its derivation is
investigated before any expectation changes (Task 3 Step 6). Step 3's
Mutation A is also the check that redefine.js's (b) and (d) reuse foo's
freed slot for `bar`: under it they must print `2` as the second value.
If they print `3`, the slot was not reused and the test does not pin the
guard through its values: change the test (not the expectation) and say
so.

- [ ] **Step 3: Prove they can fail**

One at a time on cmake-build-x86jit (rebuild after each change and
restore):
- Mutation A: in x86 `emitStringBlock`, delete the `a.jne(slowPathLab);`
  after the class-ID compare → redefine.js FAILS (OUTB: `undefined 2` or
  `1 2`; scenarios a and c print something other than `string` or
  crash), and demoted.js FAILS (CNTD: 64).
- Mutation B: in `byIdStringCanProgress`, delete the
  `if (clazz->getLazyJITId() != site.stringProtoId) return true;` lines →
  class-change.js FAILS (4 hits, NumRecompiles 1) and demoted.js FAILS
  (CNTC: 48).
- Mutation C: in `byIdStringCanProgress`, `return false;` in place of the
  final Absent/count expression → dict-add.js FAILS.
- Mutation D: in x86 `emitFastPath`, replace
  `emit_sh_ljs_is_string(a, temp1, hwSourceGpx.gpq()); a.jne(objCheckLab);`
  with `emit_sh_ljs_get_tag(a, temp1, hwSourceGpx.gpq());
  emit_sh_ljs_tag_is_pointer(a, temp1); a.jb(objCheckLab);` (any pointer
  tag takes the block) → other-prim.js FAILS.
- Mutation E: in `prepareByIdString`, make the Ineligible condition
  `id == 0 || clazz->isDictionary() || !isEligibleByIdShape(clazz,
  nullptr)` → dict.js FAILS (0 hits).
- Mutation F: in `_jit_get_by_id_refresh`, after the forwarded lookup,
  add `if (toPHV(source)->isString()) site->sawString = 1;` →
  demoted.js FAILS (CNTB: NumRecompiles 2, 8 hits).
On cmake-build-arm64 (host compilers first):
- Mutation G: in arm64 `emitStringBlock`, delete the `a.b_ne(slowPathLab);`
  after `emit_cmp_imm32` → redefine.js FAILS.

- [ ] **Step 4: All trees**

```bash
cmake --build cmake-build-x86jit --target check-hermes
cmake --build cmake-build-x86jit-hsan --target hermes HermesVMRuntimeTests &&
  cmake --build cmake-build-x86jit-hsan --target check-hermes || echo "FAIL hsan"
for t in cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  ./aarch64/qemu-sanity.sh $t &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no `FAIL` line (the hsan tree's full suite may report only the
known flake, dz `01a06156`); every getbyid-string test RUNS and passes on
all seven trees, and `JitStringPrepTest.*` passes inside the hsan tree's
`check-hermes`.

- [ ] **Step 5: Commit**

```bash
git add test/jit/getbyid-string-redefine.js \
  test/jit/getbyid-string-class-change.js \
  test/jit/getbyid-string-dict-add.js test/jit/getbyid-string-dict.js \
  test/jit/getbyid-string-demoted.js test/jit/getbyid-string-other-prim.js
git commit   # "JIT: test GetById string invalidation and demotion"
```

(Add, by path, any code file a fix in this task touched.)

---

### Task 6: doc/JIT.md

**Files:**
- Modify: `doc/JIT.md:387-415` ("Inline caches": Layout, One decoder per
  site, dump), after :415 (new paragraph), `:1157-1171` (Records),
  `:1172-1199` (Trigger), `:1206-1209` (Progress term), `:1216-1223`
  (Carry-forward), `:1269-1274` (Dump), `:1276-1298` (tests),
  `:1377-1384` (counters)

**Interfaces:**
- Consumes: the finished feature; the record size measured in Step 1.

- [ ] **Step 1: Measure the record size**

`JitByIdSiteRecord::helper` is the record's last field and 8-aligned, so
the record's size is the helper offset plus 8, and the offset is in every
GetById recording call:

```bash
for t in cmake-build-x86jit cmake-build-x86jit-hv32; do
  $t/bin/hermes -fno-inline -Xjit=force -Xdump-jitcode=3 \
    test/jit/getbyid-string-absent.js | grep -m1 'call qword ptr \[r9'
done
```

Expected (by layout: two bytes fill the hole before `helper`'s padding,
then a 2-byte ID, 2 bytes of padding and a 4-byte count): the offset grows
from 192 to 200 on HV64 and from 160 to 168 on HV32, so the record is
208 B and 176 B. Record what the dump says; if it differs, the
documented numbers follow the dump.

- [ ] **Step 2: "Inline caches"**

In the "Layout" bullet, replace its first sentence ("The non-object
check jumps straight to the slow call.") with "The non-object check jumps
straight to the slow call; at a site with a string block (see "String
receivers" below) it is a compare of the tag the string compare already
extracted." In the "One decoder per site" bullet, append: "A string
Method block, which sits before the chain, constructs and completes its
own decoder." After the dump bullet (line 415) add the paragraph:

> **String receivers** (doc/superpowers/specs/2026-10-26-jit-getbyid-string-design.md).
> A GetById-family site with a read cache entry whose record has seen a
> string (`sawString`) is prepared for strings before any of its code is
> emitted (`prepareByIdString`, shared): `length` is the Length tier; a
> data property of `String.prototype`'s own class is the Method tier,
> guarded by that class's lazy JIT ID; an absent name is Absent, and an
> accessor, a no-cache dictionary or a class without an ID is Ineligible.
> Length and Method sites put a string block in front of the object
> path: `get_tag temp1, src; cmp temp1, STRING_TAG; jne objCheck;
> <block>; objCheck: cmp temp1, OBJECT_TAG; jne slow`, so strings fall
> through untaken and objects pay one compare and one taken branch. The
> Length block masks the header's `lengthAndFlags` with
> `StringPrimitive::LENGTH_MASK`, converts it (`vcvtsi2sd`/`ucvtf`) in a
> vector temporary planned with the GP temps, and moves the bits to the
> result. The Method block loads `Runtime::stringPrototype`
> (`RuntimeOffsets::stringPrototype`; on every execution, so it follows the
> object when the GC moves it), compares its class's `lazyJITId` with the
> prepared ID (arm64: through `w16` when the ID is not an add/sub
> immediate), loads the slot and decodes it with its own
> `Emit_sh_shv_decode`. It never reads the receiver. Both end in
> `jmp contLab`; a guard miss goes to the slow call, so nothing new lies
> between the object path and the helper. Each hit counts
> `NumByIdStringHits` under `-Xjit-emit-counters`; the blocks are
> commented `// Get string length (site N)` and `// Get string method
> (site N, slot M)`. Other sites emit exactly the code above.

- [ ] **Step 3: "GetById shape records"**

- Records bullet: after "`chainEmitted`, `chainEntryEmitted`" insert
  "`sawString`, `stringTier` (`NotPrepared`, `Absent`, `Ineligible`,
  `Length` or `Method`), `stringProtoId`, `stringProtoCount`"; replace
  the sizes with Step 1's (the `JitByIdShapeEntry` sizes are unchanged).
- Trigger bullet: replace the arm64 helper offsets "192 on HV64, 160 on
  HV32" with Step 1's, and after "if neither, the chain walk follows."
  add "A string receiver's first recording call sets `sawString` and
  `changed`; later string receivers record nothing."
- Progress term bullet: append "A site also progresses on its string
  state (`byIdStringCanProgress`): it saw a string its body did not
  prepare (`NotPrepared`); or it prepared `Method`, `Absent` or
  `Ineligible` and `String.prototype`'s class now has another lazy ID
  (read without assigning one; 0 if none); or it prepared `Absent`, the
  class is a cacheable dictionary, and its property count changed (a
  dictionary gains properties in place). `Length` never progresses."
- Carry-forward bullet: add `stringTier` (to `NotPrepared`),
  `stringProtoId` and `stringProtoCount` to the reset list, and
  `sawString` to what carries.
- Dump bullet: append "When this body prepared any site as Length or
  Method, a second line follows: `JIT ById string sites: L length, M
  method`."
- Tests paragraph: append "`getbyid-string-*.js` cover the Length block
  over every string kind and both length-flag bits, Method blocks for
  every value kind and both slot placements, a site mixing a string block
  with a poly chain, a replaced method, collections between hits,
  redefinition, deletion, class changes and in-place dictionary additions,
  cacheable dictionaries, absent and inherited names, ID exhaustion, a
  large lazy ID on arm64, demoted sites, and receivers that must not take
  the block; `unittests/VMRuntime/JitStringPrepTest.cpp` covers the
  preparation, including a collection inside the lazy-ID callback."

- [ ] **Step 4: "Debugging aids"**

In the counters bullet, add `NumByIdStringHits` after `NumByIdChainHits`
in the list and to "Emitted code increments ... inline".

- [ ] **Step 5: Commit**

```bash
git add doc/JIT.md
git commit   # "JIT: document GetById on string receivers"
```

---

### Task 7: Benchmarks, Delivered section, issue

**Files:**
- Modify: `doc/superpowers/specs/2026-10-26-jit-getbyid-string-design.md` (append "Delivered")
- Close: dz `01a16b01` (`dz/issues/01a16b01-7ed6-761b-a85d-22e70cfb4c49.md`)

**Interfaces:**
- Consumes: BASE (Task 1's report), every task's report, Task 6's sizes.

Fixed work: the TypeScript benchmark is deterministic by construction:
its `Benchmark` has `doWarmup` false, `doDeterministic` true and 5
deterministic iterations (benchmarks/octane/typescript.js:418-422), each
`Measure` runs exactly those 5 (:296-310), and `RunSingleBenchmark` is
called again while fewer than `minIterations` (32) have run
(:322-330, :366-375): 35 iterations in every run, whatever the speed.

- [ ] **Step 1: Base tree**

```bash
S=/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad
mkdir -p $S/str
git -C /home/tmikov/work/hermes-x86-jit worktree add --detach $S/str/wt-base <BASE>
grep -E '^(CMAKE_BUILD_TYPE|HERMESVM_ALLOW_JIT|HERMESVM_HEAP_HV_MODE|HERMESVM_GCKIND|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|HERMES_ENABLE_DEBUGGER|CMAKE_CXX_FLAGS|CMAKE_C_FLAGS):' \
  cmake-build-jit2/CMakeCache.txt
```

Configure `$S/str/wt-base/cmake-build-jit2` with exactly those values
(`cmake -S $S/str/wt-base -B $S/str/wt-base/cmake-build-jit2 -G Ninja
-D<NAME>=<value> ...`), then build `hermes hermesc` in it and in the head
`cmake-build-jit2`. If `$S/str/wt-base` exists, check it out at BASE
instead.

- [ ] **Step 2: Run Octane**

Write `$S/str/run.sh`:

```bash
#!/bin/bash
# 5 reps; per rep and benchmark, base and head interleaved, the order
# alternating with rep parity. Release JIT trees, -Xjit, taskset -c 2.
S=/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad
R=/home/tmikov/work/hermes-x86-jit
BASE=$S/str/wt-base
P=$S/str
B="richards deltablue crypto raytrace earley-boyer regexp splay navier-stokes pdfjs mandreel gbemu box2d zlib typescript"
mkdir -p $P/hbc/base $P/hbc/head $P/logs
for b in $B; do
  $BASE/cmake-build-jit2/bin/hermesc -O -emit-binary -out $P/hbc/base/$b.hbc $R/benchmarks/octane/$b.js || exit 1
  $R/cmake-build-jit2/bin/hermesc -O -emit-binary -out $P/hbc/head/$b.hbc $R/benchmarks/octane/$b.js || exit 1
done
echo "rep,benchmark,build,score,exit" > $P/scores.csv
for rep in 1 2 3 4 5; do
  for b in $B; do
    if (( rep % 2 )); then order="base head"; else order="head base"; fi
    for build in $order; do
      if [ $build = base ]; then H=$BASE/cmake-build-jit2/bin/hermes; else H=$R/cmake-build-jit2/bin/hermes; fi
      out=$(taskset -c 2 $H -Xjit $P/hbc/$build/$b.hbc 2>&1); ec=$?
      echo "$out" > $P/logs/$rep.$b.$build.log
      score=$(echo "$out" | grep -E '^[A-Za-z0-9_ .-]+[: ]+[0-9]+(\.[0-9]+)?[[:space:]]*$' | tail -1 | grep -oE '[0-9]+(\.[0-9]+)?[[:space:]]*$' | tr -d ' ')
      echo "$rep,$b,$build,$score,$ec" >> $P/scores.csv
    done
  done
done
echo DONE > $P/done
```

and `$S/str/summ.py`:

```python
import csv, math, statistics as st, sys
P = sys.argv[1]
B = ("richards deltablue crypto raytrace earley-boyer regexp splay "
     "navier-stokes pdfjs mandreel gbemu box2d zlib typescript").split()
d, bad = {}, []
for r in csv.DictReader(open(P + "/scores.csv")):
    if r["exit"] != "0" or not r["score"]:
        bad.append(r)
        continue
    d.setdefault((r["benchmark"], r["build"]), []).append(float(r["score"]))
print("| benchmark | base median [min-max] | head median [min-max] "
      "| head/base | verdict |")
print("|---|---|---|---|---|")
rs = []
for b in B:
    o, n = d[(b, "base")], d[(b, "head")]
    mo, mn = st.median(o), st.median(n)
    rs.append(mn / mo)
    overlap = not (min(n) > max(o) or max(n) < min(o))
    v = "noise" if overlap else ("faster" if mn > mo else "slower")
    print(f"| {b} | {mo:g} [{min(o):g}-{max(o):g}] "
          f"| {mn:g} [{min(n):g}-{max(n):g}] | {mn / mo:.3f} | {v} |")
print(f"| geomean | | | {math.exp(sum(map(math.log, rs)) / len(rs)):.3f} | |")
if bad:
    print("BAD ROWS:", bad)
print("samples per cell:", sorted(set(len(v) for v in d.values())))
```

Run, with nothing else building or testing:

```bash
bash $S/str/run.sh
python3 -I $S/str/summ.py $S/str
```

Expected (spec "Benchmarks"): TypeScript faster beyond noise; no
benchmark slower beyond noise; 5 samples per cell; no BAD ROWS. Results
that miss expectations are reported as measured, with a profile, not
tuned away. (The interpreter is not run: no interpreter source changed.)

- [ ] **Step 3: String-receiver slow calls in TypeScript**

Temporary instrumentation (never committed) in the head tree's
`lib/VM/StaticH.cpp`: add `#include <cstdio>` after `#include <cstdarg>`
(line 32); immediately before `_sh_ljs_get_by_id_rjs` (line 1418) add

```cpp
// TEMPORARY: string-receiver GetById slow calls, printed at exit.
namespace {
struct StrGetByIdCounts {
  unsigned long long total = 0, length = 0, charCodeAt = 0;
  ~StrGetByIdCounts() {
    fprintf(
        stderr,
        "STRGETBYID total %llu length %llu charCodeAt %llu\n",
        total,
        length,
        charCodeAt);
  }
} strGetByIdCounts;
} // namespace
```

and as the first statement of `_sh_ljs_get_by_id_rjs`'s body

```cpp
  if (toPHV(source)->isString()) {
    ++strGetByIdCounts.total;
    if (symID == Predefined::getSymbolID(Predefined::length).unsafeGetRaw())
      ++strGetByIdCounts.length;
    else if (
        symID ==
        Predefined::getSymbolID(Predefined::charCodeAt).unsafeGetRaw())
      ++strGetByIdCounts.charCodeAt;
  }
```

Every JIT GetById slow call reaches this function exactly once: the
recording helper and the refresh helper forward to it
(JitHandlers.cpp:990, :1042), a demoted slot calls it directly, and the
interpreter never calls it. Then:

```bash
git diff lib/VM/StaticH.cpp > $S/str/strget.diff
git -C $S/str/wt-base apply $S/str/strget.diff
cmake --build cmake-build-jit2 --target hermes
cmake --build $S/str/wt-base/cmake-build-jit2 --target hermes
taskset -c 2 cmake-build-jit2/bin/hermes -Xjit $S/str/hbc/head/typescript.hbc 2>&1 | grep STRGETBYID
taskset -c 2 $S/str/wt-base/cmake-build-jit2/bin/hermes -Xjit $S/str/hbc/base/typescript.hbc 2>&1 | grep STRGETBYID
```

Run each twice; the totals must repeat exactly (fixed work). If no
`STRGETBYID` line appears (the process ended without running static
destructors), print the same line from a function registered with
`std::atexit` on the first count instead. Expected:
head's total below 10% of base's (dz `01a16b01` measured 22.2M at
e5c158faa). Then remove the instrumentation from both trees
(`git checkout lib/VM/StaticH.cpp` and
`git -C $S/str/wt-base checkout lib/VM/StaticH.cpp`) and rebuild both
`cmake-build-jit2` trees; `git status` must show no modified tracked
file.

- [ ] **Step 4: Delivered section**

Append to the end of the spec
(`doc/superpowers/specs/2026-10-26-jit-getbyid-string-design.md`), with
the date filled in:

```markdown
## Delivered (YYYY-MM-DD)

Commits `<first>..<last>` on branch `x86-jit` (base `<BASE>`).

### Differences from the spec
(one bullet per spec resolution below, plus anything the implementation
changed, each with its reason)

### Matrix
(table: tree | configuration | result, from the task reports: full
check-hermes on cmake-build-x86jit and cmake-build-x86jit-hsan; the JIT
suite on -hv32 and -boxed; the JIT suite and qemu-sanity on the three
arm64 trees; then each task's mutations and what each failed)

### Sizes
(JitByIdSiteRecord and its helper offset, HV64 and HV32, before (200/192,
168/160) and after, from Task 6 Step 1)

### Benchmarks
(the exact commands; the JIT table of Step 2 with the geomean; then
TypeScript's string-receiver slow calls, base and head totals and the
length and charCodeAt parts, and their ratio)
```

Every value comes from a run made in this task or a task report;
nothing is estimated.

- [ ] **Step 5: Close the issue**

If TypeScript met the spec's expectations:

```bash
(cd /home/tmikov/work/hermes-x86-jit && dz close 01a16b01 --as fixed -m - --json <<'EOF'
GetById on string receivers is served inline on x86-64 and arm64
(doc/superpowers/specs/2026-10-26-jit-getbyid-string-design.md; plan
"Delivered"):
`length` from the header, String.prototype data properties through a
class-ID guarded slot load. TypeScript string-receiver slow calls:
<base total> -> <head total>; JIT score <base median> -> <head median>.
EOF
)
```

with the measured numbers filled in. Otherwise run
`(cd /home/tmikov/work/hermes-x86-jit && dz comment 01a16b01 -m - --json <<'EOF' ... EOF)`
with the same measurements and what remains, and leave it open.

- [ ] **Step 6: Commit**

```bash
git add doc/superpowers/specs/2026-10-26-jit-getbyid-string-design.md \
  dz/issues/01a16b01-7ed6-761b-a85d-22e70cfb4c49.md
git commit   # "JIT: record the delivered GetById string measurements"
```

Optionally remove the base worktree afterwards:
`git -C /home/tmikov/work/hermes-x86-jit worktree remove --force $S/str/wt-base`.

---

## Spec resolutions (summary)

1. `prepareByIdString` is defined after `jitWalkChain` in
   JitHandlers.cpp, not next to `prepareByIdPolyChain`: it uses the
   `static` `chainClassLookup`, defined after the latter (Task 1). Its
   declaration is next to `prepareByIdPolyChain`'s.
2. The string terms of `byIdSiteCanProgress` live in a JitHandlers.cpp
   function, `byIdStringCanProgress` (Task 2).
3. `RuntimeOffsets::stringPrototype` also asserts 8-byte alignment,
   which arm64's `emit_load_from_base_offset<8>` requires (Task 1).
4. Interim states: Task 2 prepares and records tiers on both backends
   but emits no block (Length never progresses again, Method only on a
   class change: no loop); Task 3 emits on x86-64 with the new tests
   x86-only; Task 4 lifts both.
5. The dump's separate line still breaks test/jit/x86-64/exceptions.js,
   whose `--match-full-lines` checks follow `JIT ById sites` with
   `CHECK-NEXT` (:385-386, :393-394): Task 2 adds the string line there.
6. Scenarios that each need a pristine `String.prototype` (redefine,
   demoted) run one per process, selected with a script argument through
   `hermescli.getScriptArgs()`.
7. The demoted test adds a second recording site T so that a demoted
   site that wrongly recorded strings would recompile (scenario b has a
   refresh-helper mutation); scenario a still has no mutation in this
   plan's code: it passes before the feature by design.
8. Tests beyond the spec's table: `lenSelf` (length.js), the `toString`
   and `indexOf` slot checks (method.js and the unit test),
   getbyid-string-bigid.js, and the BigInt, symbol, String-object and
   throwing receivers of other-prim.js (Review Focus).
9. The Delivered section goes into the spec, as for the chain spec
   (controller ruling); this plan keeps no Delivered section.

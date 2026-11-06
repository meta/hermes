# JIT Recompilation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the JIT compile a function again after its first compile and
install the new body for future invocations; v1's consumer recompiles
functions whose ById property caches were cold at first compile.

**Architecture:** Per-function `JitFunctionData` metadata hangs off
CodeBlock; the `_jit_put_by_id` helper counts declines and, past a
threshold, asks `JITContext` to recompile when the caches have warmed;
install is a plain `setJITCompiled()` store (every call site loads the
pointer per call); retired bodies are kept alive (owned by
`JITContext::Impl`, as all code already is) and bounded by a per-function
budget.

**Tech Stack:** C++17, x86-64 JIT backend (asmjit), lit/FileCheck tests.

**Spec:** `doc/superpowers/specs/2026-09-04-jit-recompilation-design.md`

## Global Constraints

- Work in `/home/tmikov/work/hermes-x86-jit`, branch `x86-jit`. Never `cd`
  away from the repo root; pass paths to commands instead.
- Build/test tree: `cmake-build-x86jit` (ASan+Debug, already configured).
  Build: `cmake --build cmake-build-x86jit --target hermes`.
  Run jit tests: `LIT_OPTS="-j8" LIT_FILTER="jit" cmake --build
  cmake-build-x86jit --target check-hermes`.
- C++17, no exceptions/RTTI; 80-column lines; 2-space indent; doc comment
  on every declaration; member variables have a `_` suffix only in
  classes (the plain `JitFunctionData` struct uses bare names).
- Every new file starts with the standard Meta copyright header (copy it
  from any neighboring file).
- Any change under `lib/VM/` or `include/hermes/VM/`: invoke the
  `gc-safe-coding` skill first and follow it. The trigger code in this
  plan deliberately runs before any raw object pointer is derived.
- Do NOT modify the shared `_sh_ljs_*` helpers in `lib/VM/StaticH.cpp` —
  they are shared with shermes-AOT output. Only `_jit_*` helpers in
  `lib/VM/JIT/JitHandlers.cpp` may learn about recompilation.
- arm64: the only arm64 file touched is `include/hermes/VM/JIT/arm64/JIT.h`
  (a trivial setter so shared code compiles). No arm64 emitter changes.
- Each task ends with: jit lit suite green on `cmake-build-x86jit`, then
  commit with the message given in the task.

---

### Task 1: `JitFunctionData` and the CodeBlock field

**Files:**
- Create: `include/hermes/VM/JIT/JitFunctionData.h`
- Modify: `include/hermes/VM/CodeBlock.h` (fields at ~line 65–83,
  accessors near `getJITCompiled()` at ~line 275)
- Modify: `lib/VM/CodeBlock.cpp` (include + destructor definition)

**Interfaces:**
- Produces: `struct hermes::vm::JitFunctionData` with public fields
  `uint8_t recompileBudget`, `uint8_t version`, `uint16_t coldByIdSites`,
  `uint32_t declineCount`,
  `llvh::SmallVector<JITCompiledFunctionPtr, 1> retired`, and constant
  `static constexpr uint32_t kRecompileDeclineThreshold = 64;`.
- Produces: `CodeBlock::getJitData() -> JitFunctionData *` (null until
  first compile) and
  `CodeBlock::ensureJitData(uint8_t budget) -> JitFunctionData *`
  (allocates on first call with `recompileBudget = budget`,
  `version = 1`; later calls return the existing object unchanged).

- [ ] **Step 1: Create the header**

`include/hermes/VM/JIT/JitFunctionData.h` (after the copyright header):

```cpp
#ifndef HERMES_VM_JIT_JITFUNCTIONDATA_H
#define HERMES_VM_JIT_JITFUNCTIONDATA_H

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#include "llvh/ADT/SmallVector.h"

#include <cstdint>

namespace hermes {
namespace vm {

class Runtime;
typedef HermesValue (*JITCompiledFunctionPtr)(Runtime *runtime);

/// Per-function JIT metadata supporting recompilation. Allocated lazily
/// when a function is first JIT-compiled; owned by the CodeBlock; see
/// doc/superpowers/specs/2026-09-04-jit-recompilation-design.md.
struct JitFunctionData {
  /// Declines of the ById helpers before the recompile check runs.
  static constexpr uint32_t kRecompileDeclineThreshold = 64;

  /// Remaining recompiles for this function. 0 disables all triggering.
  uint8_t recompileBudget;
  /// 1 after the first compile; incremented on each installed recompile.
  uint8_t version = 1;
  /// Number of Get/PutById sites whose inline tier was skipped by the
  /// most recent compile because the property cache was cold. Written by
  /// the emitter after every compile; a recompile can only help when
  /// this is non-zero.
  uint16_t coldByIdSites = 0;
  /// Helper-side decline counter; reset when a version is installed.
  uint32_t declineCount = 0;
  /// Previous bodies, retired by recompiles. Bookkeeping only: the code
  /// itself is owned by JITContext::Impl and is never freed (see the
  /// reclamation dz issue). A frame may still be executing one of these.
  llvh::SmallVector<JITCompiledFunctionPtr, 1> retired{};
  /// Reserved for consumer-specific feedback records (e.g. the future
  /// PutByVal per-site observed-kind records). Always null in v1; typed
  /// and owned by the consumer that allocates it.
  void *consumerRecords = nullptr;

  /// \param budget initial recompile budget (from -Xjit-max-recompiles).
  explicit JitFunctionData(uint8_t budget) : recompileBudget(budget) {}
};

} // namespace vm
} // namespace hermes

#endif // HERMESVM_JIT
#endif // HERMES_VM_JIT_JITFUNCTIONDATA_H
```

Note: `JITCompiledFunctionPtr` is also typedef'ed in CodeBlock.h:35 —
an identical typedef in both headers is legal C++ and avoids an include
cycle. Do NOT include CodeBlock.h here. `HermesValue` needs
`#include "hermes/VM/HermesValue.h"` — add it.

- [ ] **Step 2: Add the CodeBlock field and accessors**

In `include/hermes/VM/CodeBlock.h`: forward-declare above the class
(next to `class CodeBlock;` at line ~32):

```cpp
struct JitFunctionData;
```

Next to `JITCompiled_` (line ~68), inside the class:

```cpp
#if HERMESVM_JIT
  /// Lazily allocated recompilation metadata; null until first compile.
  /// See JitFunctionData.h. unique_ptr of an incomplete type: the
  /// out-of-line ~CodeBlock() in CodeBlock.cpp sees the full type.
  std::unique_ptr<JitFunctionData> jitData_{};
#endif
```

Declare the destructor in the public section (CodeBlock currently has no
declared destructor):

```cpp
  ~CodeBlock();
```

Next to `getJITCompiled()`/`setJITCompiled()` in the `#if HERMESVM_JIT`
block (~line 275), add:

```cpp
  /// \return the recompilation metadata, or null if never JIT-compiled.
  JitFunctionData *getJitData() {
    return jitData_.get();
  }

  /// Allocate the recompilation metadata on first use.
  /// \param budget the initial recompile budget for this function.
  /// \return the metadata (existing object if already allocated).
  JitFunctionData *ensureJitData(uint8_t budget);
```

In the `#else` (no-JIT) block add the stubs:

```cpp
  JitFunctionData *getJitData() {
    return nullptr;
  }
  JitFunctionData *ensureJitData(uint8_t budget) {
    return nullptr;
  }
```

- [ ] **Step 3: Define destructor and ensureJitData in CodeBlock.cpp**

In `lib/VM/CodeBlock.cpp`, add
`#include "hermes/VM/JIT/JitFunctionData.h"` with the other includes,
and near the top of the implementation:

```cpp
CodeBlock::~CodeBlock() = default;

#if HERMESVM_JIT
JitFunctionData *CodeBlock::ensureJitData(uint8_t budget) {
  if (!jitData_)
    jitData_.reset(new JitFunctionData(budget));
  return jitData_.get();
}
#endif
```

- [ ] **Step 4: Build and run the jit lit suite**

Run: `cmake --build cmake-build-x86jit --target hermes` then
`LIT_OPTS="-j8" LIT_FILTER="jit" cmake --build cmake-build-x86jit
--target check-hermes`.
Expected: build clean, suite green (no behavior change yet).

- [ ] **Step 5: Commit**

```bash
git add include/hermes/VM/JIT/JitFunctionData.h include/hermes/VM/CodeBlock.h lib/VM/CodeBlock.cpp
git commit -m "JIT: add per-function recompilation metadata (JitFunctionData)"
```

---

### Task 2: `-Xjit-max-recompiles` flag plumbing

**Files:**
- Modify: `include/hermes/VM/RuntimeFlags.h` (next to `JITMemoryLimit`,
  ~line 238)
- Modify: `public/hermes/Public/RuntimeConfig.h` (next to
  `JITMemoryLimit`, line ~141)
- Modify: `tools/hermes/hermes.cpp` (builder chain, line ~118)
- Modify: `lib/VM/Runtime.cpp` (config application, line ~488)
- Modify: `include/hermes/VM/JIT/x86-64/JIT.h`,
  `include/hermes/VM/JIT/arm64/JIT.h` (setter + member, next to
  `setMemoryLimit`), and the no-JIT stub `JITContext` in
  `include/hermes/VM/JIT/JIT.h` (no-op setter)

**Interfaces:**
- Produces: `JITContext::setMaxRecompiles(uint8_t)` and
  `JITContext::getMaxRecompiles() -> uint8_t` (default 1), on the real
  x86-64/arm64 contexts and as a no-op/0 on the stub.
- Produces: runtime flag `-Xjit-max-recompiles=N` and RuntimeConfig field
  `JITMaxRecompiles` flowing into the JITContext.

- [ ] **Step 1: Add the CLI flag**

`include/hermes/VM/RuntimeFlags.h`, after the `JITMemoryLimit` opt:

```cpp
  llvh::cl::opt<uint32_t> JITMaxRecompiles{
      "Xjit-max-recompiles",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("maximum number of recompiles per function "
                     "(0 disables recompilation)"),
      llvh::cl::init(1)};
```

- [ ] **Step 2: Add the RuntimeConfig field**

`public/hermes/Public/RuntimeConfig.h`, after the `JITMemoryLimit` line
in the field macro list (keep the trailing backslash column aligned):

```cpp
  /* Maximum recompiles per function; 0 disables recompilation. */      \
  F(constexpr, uint32_t, JITMaxRecompiles, 1)                           \
```

- [ ] **Step 3: Wire flag -> config -> JITContext**

`tools/hermes/hermes.cpp` line ~119, after `.withJITMemoryLimit(...)`:

```cpp
          .withJITMaxRecompiles(flags.JITMaxRecompiles)
```

`lib/VM/Runtime.cpp` line ~489, after `setMemoryLimit(...)`:

```cpp
  jitContext_.setMaxRecompiles(
      std::min<uint32_t>(runtimeConfig.getJITMaxRecompiles(), 255));
```

- [ ] **Step 4: Add the JITContext member and setter**

In `include/hermes/VM/JIT/x86-64/JIT.h`, next to `setMemoryLimit()`:

```cpp
  /// Set the maximum number of recompiles per function (0 disables).
  void setMaxRecompiles(uint8_t maxRecompiles) {
    maxRecompiles_ = maxRecompiles;
  }

  /// \return the maximum number of recompiles per function.
  uint8_t getMaxRecompiles() const {
    return maxRecompiles_;
  }
```

and in the private member area (next to `memoryLimit_`):

```cpp
  /// Maximum number of recompiles per function. 0 disables recompilation.
  uint8_t maxRecompiles_{1};
```

Mirror the same three additions verbatim in
`include/hermes/VM/JIT/arm64/JIT.h`. In the no-JIT stub `JITContext`
inside `include/hermes/VM/JIT/JIT.h`, add:

```cpp
  void setMaxRecompiles(uint8_t maxRecompiles) {}
  uint8_t getMaxRecompiles() const {
    return 0;
  }
```

- [ ] **Step 5: Build, verify flag parses, run suite**

Run: `cmake --build cmake-build-x86jit --target hermes` and
`cmake-build-x86jit/bin/hermes -Xjit-max-recompiles=0 -Xjit
/dev/null`.
Expected: build clean, no "unknown argument" error, suite green.

- [ ] **Step 6: Commit**

```bash
git add include/hermes/VM/RuntimeFlags.h public/hermes/Public/RuntimeConfig.h tools/hermes/hermes.cpp lib/VM/Runtime.cpp include/hermes/VM/JIT/JIT.h include/hermes/VM/JIT/x86-64/JIT.h include/hermes/VM/JIT/arm64/JIT.h
git commit -m "JIT: add -Xjit-max-recompiles flag (default 1), plumbed to JITContext"
```

---

### Task 3: emitter reports cold ById sites; metadata filled at install

**Files:**
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` (Emitter public member area)
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (PutById gate at
  ~line 1085–1100; GetById specialization gate at ~line 620–637)
- Modify: `lib/VM/JIT/JitCompiler.cpp`
  (`Compiler::compileCodeBlockImpl()`, install region at ~line 329, and
  the CompileStatus dump at ~line 362)
- Test: `test/jit/x86-64/recompile-cold-sites.js`

**Interfaces:**
- Consumes: `CodeBlock::ensureJitData(uint8_t)` and `JitFunctionData`
  fields from Task 1; `JITContext::getMaxRecompiles()` from Task 2.
- Produces: `Emitter::coldByIdSites_` (public `uint32_t`, zeroed at
  construction), incremented once per ById site whose tier/specialization
  was skipped for a cold cache; after every successful compile the
  CodeBlock's `JitFunctionData` exists and holds fresh `coldByIdSites`.
- Produces: with `-Xdump-jitcode` CompileStatus, the success line is
  followed by `JIT cold ById sites: N` when N > 0.

- [ ] **Step 1: Add the emitter counter**

In `lib/VM/JIT/x86-64/JitEmitter.h`, in the Emitter's public data area
(near other public members such as `emittingIP`):

```cpp
  /// Number of Get/PutById sites in this function whose inline
  /// tier/specialization was skipped because the property cache had no
  /// class yet. Read by the compile driver into JitFunctionData; a
  /// recompile can only improve the function when this is non-zero.
  uint32_t coldByIdSites_ = 0;
```

- [ ] **Step 2: Count the cold PutById sites**

In `lib/VM/JIT/x86-64/JitEmitter-property.cpp`, in the PutById
implementation (~line 1088), the tier is gated on `clazzID`:

```cpp
  uint16_t clazzID = 0;
  SlotIndex slot = 0;
  if (cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
    ...
    clazzID = initHCLazyIDMayAlloc(...);
  }
```

Immediately after that `if` block, add:

```cpp
  // A valid cache index with no cached class is a site a recompile can
  // upgrade once the cache warms.
  if (cacheIdx != hbc::PROPERTY_CACHING_DISABLED && !clazzID)
    ++coldByIdSites_;
```

Note the existing code wraps this region in
`#if HERMES_JIT_INLINE_SAFE_STORE`; the new increment goes inside the
same conditional block.

- [ ] **Step 3: Count the cold GetById sites**

Same file, GetById specialization gate (~line 620). The specialized
fast path is emitted only when
`cacheEntry->numGoodChanges == 1 && cacheEntry->clazz` is set. Extend
the site to count the cold case — restructure minimally:

```cpp
    if (ReadPropertyCacheEntry *cacheEntry =
            _.codeBlock_->getReadCacheEntry(cacheIdx);
        cacheEntry->numGoodChanges == 1) {
      ...existing body unchanged...
    } else if (!_.codeBlock_->getReadCacheEntry(cacheIdx)
                    ->clazz.getNoBarrierUnsafe()) {
      // Cold cache: no specialization possible yet. A recompile after
      // the cache warms can upgrade this site.
      ++_.coldByIdSites_;
    }
```

(`_` is the enclosing emitter reference already used on the adjacent
lines; keep whatever the local convention is at that site.)

- [ ] **Step 4: Fill metadata at install and print the count**

In `lib/VM/JIT/JitCompiler.cpp`,
`Compiler::compileCodeBlockImpl()`, right after
`codeBlock_->setJITCompiled(em_.addToRuntime(jc_.impl_->jr));`
(line ~329):

```cpp
  // Create or refresh the recompilation metadata. The budget is set only
  // on first allocation; a recompile must not replenish it.
  JitFunctionData *jitData =
      codeBlock_->ensureJitData(jc_.getMaxRecompiles());
  jitData->coldByIdSites =
      em_.coldByIdSites_ > 0xffff ? 0xffff : em_.coldByIdSites_;
  jitData->declineCount = 0;
```

Add `#include "hermes/VM/JIT/JitFunctionData.h"` to the file's includes.
(`jc_` is the `JITContext &`; if `getMaxRecompiles()` is private-only,
it was declared public in Task 2.)

In the CompileStatus dump block (~line 362), after the
"JIT successfully compiled" line:

```cpp
    if (em_.coldByIdSites_ > 0) {
      llvh::outs() << "JIT cold ById sites: " << em_.coldByIdSites_
                   << "\n";
    }
```

- [ ] **Step 5: Write the lit test**

`test/jit/x86-64/recompile-cold-sites.js` (copyright header, then):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xdump-jitcode=2 %s | %FileCheck --check-prefix=FORCE %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xdump-jitcode=2 %s | %FileCheck --check-prefix=WARM %s
// REQUIRES: jit

// Under -Xjit=force the function compiles before it ever runs, so the
// write cache at `o.p = v` is cold and the site is reported. Under a
// threshold, the interpreter warms the cache first and no cold site is
// reported for f.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 20; ++i)
  f(o, i);
print(o.p);

// FORCE: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// FORCE: JIT cold ById sites: {{[0-9]+}}
// FORCE: 19

// WARM: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// WARM-NOT: JIT cold ById sites
// WARM: 19
```

Caution: under WARM, other functions (the top-level) may legitimately
report cold sites; scope the `WARM-NOT` between f's banner and f's
"successfully compiled" line by adding
`// WARM: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'` after
the `WARM-NOT`. Verify the actual dump ordering when writing the test
and pin what is really printed.

- [ ] **Step 6: Build, run the new test, then the jit suite**

Run: `cmake --build cmake-build-x86jit --target hermes` then
`LIT_OPTS="-j8" LIT_FILTER="recompile-cold-sites" cmake --build
cmake-build-x86jit --target check-hermes`, then the full jit filter.
Expected: new test passes; suite green.

- [ ] **Step 7: Commit**

```bash
git add lib/VM/JIT/x86-64/JitEmitter.h lib/VM/JIT/x86-64/JitEmitter-property.cpp lib/VM/JIT/JitCompiler.cpp test/jit/x86-64/recompile-cold-sites.js
git commit -m "JIT: x86-64: report ById sites left cold by a compile"
```

---

### Task 4: recompile entry point, helper trigger, and the headline test

**Files:**
- Modify: `include/hermes/VM/JIT/x86-64/JIT.h` (declare `recompile` and
  `considerRecompile` next to `compileImpl`; both public)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (implement both; version banner)
- Modify: `lib/VM/JIT/JitHandlers.cpp` (`_jit_put_by_id`, line ~311)
- Modify: `include/hermes/VM/JIT/JitCounters.h` (two new counters)
- Test: `test/jit/x86-64/recompile-byid-warm.js`

**Interfaces:**
- Consumes: `JitFunctionData` (Task 1), `getMaxRecompiles()` (Task 2),
  metadata filled at install (Task 3).
- Produces: `JITContext::recompile(Runtime &, CodeBlock *) -> bool`
  (true if a new version was installed) and
  `JITContext::considerRecompile(Runtime &, CodeBlock *)` (the
  threshold-crossing check; safe to call from JIT helpers).
- Produces: dump banner suffix ` (version N)` for N >= 2 on both the
  "JIT compilation of" and "JIT successfully compiled" lines; perf
  jitdump symbol suffix ` v2` etc.

- [ ] **Step 1: Add the counters**

`include/hermes/VM/JIT/JitCounters.h`:

```cpp
#define JIT_COUNTERS(X) \
  X(NumCall)            \
  X(NumCallSlow)        \
  X(NumRecompileChecks) \
  X(NumRecompiles)
```

- [ ] **Step 2: Declare the entry points**

`include/hermes/VM/JIT/x86-64/JIT.h`, in the public section:

```cpp
  /// Compile \p codeBlock again, reading the current property-cache
  /// state, and install the new body for future invocations. The
  /// previous body is retired (kept alive; see JitFunctionData) and the
  /// recompile budget is decremented. On compilation failure the budget
  /// is zeroed so the function is never retried.
  /// \pre codeBlock has been JIT-compiled (getJITCompiled() non-null).
  /// \return true if a new version was installed.
  bool recompile(Runtime &runtime, CodeBlock *codeBlock);

  /// Called by JIT runtime helpers when a function's decline counter
  /// crosses JitFunctionData::kRecompileDeclineThreshold. Spends budget
  /// only when progress is possible: the last compile left cold ById
  /// sites and at least one property cache entry has warmed since.
  /// Otherwise resets the decline counter and returns.
  void considerRecompile(Runtime &runtime, CodeBlock *codeBlock);
```

- [ ] **Step 3: Implement in JitCompiler.cpp**

Add near `compileImpl` (helper for counter bumps: `counters_` is a
member; bump only when allocated):

```cpp
bool JITContext::recompile(Runtime &runtime, CodeBlock *codeBlock) {
  assert(
      codeBlock->getJITCompiled() &&
      "recompile requires an existing compiled body");
  JitFunctionData *data = codeBlock->getJitData();
  assert(data && "compiled function must have JitFunctionData");

  JITCompiledFunctionPtr old = codeBlock->getJITCompiled();
  // compileImpl installs the new body via setJITCompiled() itself and
  // refreshes coldByIdSites/declineCount (Task 3 code). It returns null
  // on failure, in which case nothing was installed and the old body
  // remains current.
  JITCompiledFunctionPtr res = compileImpl(runtime, codeBlock);
  if (!res) {
    data->recompileBudget = 0;
    return false;
  }
  data->retired.push_back(old);
  --data->recompileBudget;
  ++data->version;
  if (counters_)
    ++counters_.get()[(unsigned)JitCounter::NumRecompiles];
  return true;
}

void JITContext::considerRecompile(Runtime &runtime, CodeBlock *codeBlock) {
  JitFunctionData *data = codeBlock->getJitData();
  assert(data && "considerRecompile requires JitFunctionData");
  if (counters_)
    ++counters_.get()[(unsigned)JitCounter::NumRecompileChecks];
  data->declineCount = 0;
  if (!enabled_ || !data->recompileBudget || data->coldByIdSites == 0)
    return;
  // Progress check: has any property cache warmed since the compile?
  bool warmed = false;
  for (unsigned i = 0, e = codeBlock->getWriteCacheSize(); i < e; ++i) {
    if (codeBlock->getWriteCacheEntry(i)->clazz.getNoBarrierUnsafe()) {
      warmed = true;
      break;
    }
  }
  if (!warmed) {
    for (unsigned i = 0, e = codeBlock->getReadCacheSize(); i < e; ++i) {
      if (codeBlock->getReadCacheEntry(i)->clazz.getNoBarrierUnsafe()) {
        warmed = true;
        break;
      }
    }
  }
  if (!warmed)
    return;
  recompile(runtime, codeBlock);
}
```

Check the real accessor names for the cache sizes in CodeBlock.h
(~line 110: they are `getReadPropertyCacheSize()` /
`getWritePropertyCacheSize()` or similar — use the existing names; the
entry accessors are `getReadCacheEntry`/`getWriteCacheEntry`). If the
cache-size getters are private or absent, add public
`getReadPropertyCacheSize()`/`getWritePropertyCacheSize()` returning the
existing `readPropertyCacheSize_`/`writePropertyCacheSize_` fields.

- [ ] **Step 4: Version labels in dumps**

In `Compiler::compileCodeBlockImpl()`: the banner (~line 276) and the
success line (~line 363) both print
`FunctionID ... , 'name'`. Compute once at the top of the function:

```cpp
  JitFunctionData *priorData = codeBlock_->getJitData();
  unsigned version = priorData ? priorData->version + 1u : 1u;
```

and change both prints to append the version when `version >= 2`:

```cpp
    llvh::outs() << "\nJIT compilation of FunctionID "
                 << codeBlock_->getFunctionID() << ", '" << funcName_
                 << "'";
    if (version >= 2)
      llvh::outs() << " (version " << version << ")";
    llvh::outs() << "\n";
```

(and the analogous change on the "JIT successfully compiled" line).
For the perf jitdump record (~line 333), when `version >= 2` pass
`codeBlock_->getNameString() + " v" + std::to_string(version)` instead
of the bare name.

- [ ] **Step 5: The helper trigger**

`lib/VM/JIT/JitHandlers.cpp`, at the very top of `_jit_put_by_id`
(line ~311), before any other statement — the recompile can move the
GC heap, so it must run before any raw object pointer is derived, and
here only frame-slot pointers exist:

```cpp
  // Recompilation trigger: every call to this helper is a decline of
  // the inline PutById tier (absent or guard-missed). Threshold
  // crossings hand off to the JITContext, which recompiles only when
  // the caches have warmed enough to make progress. Runs before any
  // raw object pointer is derived: recompilation may allocate.
  if (JitFunctionData *data = ((CodeBlock *)codeBlock)->getJitData();
      LLVM_UNLIKELY(
          data && data->recompileBudget &&
          ++data->declineCount >=
              JitFunctionData::kRecompileDeclineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(
        getRuntime(shr), (CodeBlock *)codeBlock);
  }
```

Add `#include "hermes/VM/JIT/JitFunctionData.h"` to the includes.
Note `considerRecompile` always resets `declineCount`, so the counter
cannot re-trigger on every subsequent call once the budget is spent.

- [ ] **Step 6: Write the headline lit test**

`test/jit/x86-64/recompile-byid-warm.js` (copyright header, then):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=SPEC %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=0 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=OFF %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Under -Xjit=force, f compiles before it ever runs: its write cache is
// cold and the PutById inline tier is not emitted (the known force-mode
// gap). The helper calls warm the cache and count declines; past the
// threshold (64) the function is recompiled and version 2 carries the
// tier. With -Xjit-max-recompiles=0 no version 2 may appear.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 100; ++i)
  f(o, i);
print(o.p);

// OUT: 99

// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f'
// SPEC: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: // Put to object specialization
// SPEC: JIT successfully compiled FunctionID {{[0-9]+}}, 'f' (version 2)
// SPEC: 99

// OFF-NOT: (version
// OFF: 99
```

The `// Put to object specialization` comment is the PutById inline
tier's banner (see `emitPutByIdInlineTier`); its presence after the
version-2 banner pins that the recompile emitted the tier. Verify the
exact dump ordering by running the command by hand first and adjust the
CHECK lines to what is actually printed — but the three essentials must
stay: no tier in version 1, a version-2 compile happens, the tier is in
version 2, and OFF has no version line at all.

- [ ] **Step 7: Prove the test can fail**

Run the SPEC command with `-Xjit-max-recompiles=0` manually and confirm
FileCheck fails on the missing `(version 2)` line. Then run the real
test: `LIT_OPTS="-j8" LIT_FILTER="recompile-byid-warm" cmake --build
cmake-build-x86jit --target check-hermes`.
Expected: manual mutation fails, real test passes.

- [ ] **Step 8: Full jit suite, then commit**

Run the full jit LIT_FILTER suite. Expected: green — in particular the
existing `-emitted` pin tests still pass because they compile at
`-Xjit-threshold=4` with warm caches (`coldByIdSites == 0`, no trigger).

```bash
git add include/hermes/VM/JIT/x86-64/JIT.h lib/VM/JIT/JitCompiler.cpp lib/VM/JIT/JitHandlers.cpp include/hermes/VM/JIT/JitCounters.h test/jit/x86-64/recompile-byid-warm.js
git commit -m "JIT: recompile functions whose ById caches were cold at first compile"
```

---

### Task 5: stress tests, determinism, and baseline-workflow guard

**Files:**
- Test: `test/jit/x86-64/recompile-mid-recursion.js`
- Test: `test/jit/x86-64/recompile-deterministic.js`
- Modify: `utils/jit/jit-dump.sh` (add `-Xjit-max-recompiles=0` to the
  hermes invocation's canonical flag set)

**Interfaces:**
- Consumes: the complete mechanism from Task 4.

- [ ] **Step 1: Old-body-on-stack test**

`test/jit/x86-64/recompile-mid-recursion.js` (copyright header, then):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s | %FileCheck %s
// REQUIRES: jit

// The recursion performs one PutById decline per level. The decline
// threshold (64) is crossed deep inside a single outer invocation, so
// the recompile installs version 2 while ~64 frames of version 1 are
// still on the native stack; they must unwind through the retired body
// correctly. Run on the ASan build tree, where a freed or corrupted
// retired body would be caught.

function r(o, n) {
  o.p = n;
  if (n > 0)
    r(o, n - 1);
  return o.p;
}

print(r({p: -1}, 100));
// CHECK: 0
```

- [ ] **Step 2: Determinism test**

`test/jit/x86-64/recompile-deterministic.js` (copyright header, then):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xdump-jitcode=3 %s > %t.1
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xdump-jitcode=3 %s > %t.2
// RUN: diff %t.1 %t.2
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Recompilation must not make emission nondeterministic: two identical
// runs produce byte-identical dumps, version-2 bodies included.

function f(o, v) {
  o.p = v;
}
var o = {p: 0};
for (var i = 0; i < 100; ++i)
  f(o, i);
print(o.p);
```

Caveat: dumps contain absolute addresses (e.g. helper-call targets
`mov r11, 0x...`) only if ASLR varies between runs; both runs are the
same binary in the same lit invocation but separate processes.
If `diff` fails on address lines only, switch both RUN lines to filter:
`... | sed 's/0x[0-9A-Fa-f]*//g' > %t.1` and likewise `%t.2`, keeping
the structural comparison. Note in the test comment why the filter
exists.

- [ ] **Step 3: Guard the byte-identity baseline workflow**

In `utils/jit/jit-dump.sh`, find the hermes invocation that produces the
baseline dump and add `-Xjit-max-recompiles=0` to its flags, with a
comment:

```bash
# Recompilation would put multiple bodies per function in the dump;
# baselines are defined as first-compile output.
```

Verify: run `utils/jit/jit-dump.sh -o /tmp/rt.dump
cmake-build-x86jit/bin/hermes` twice (before and after the edit, same
binary) and confirm the dump is unchanged — today nothing triggers in
the suite it runs, so this is a guard, not a behavior change. Read
`utils/jit/README.md` first if the script's flag plumbing is unclear.

- [ ] **Step 4: Run the wider gates**

- Full jit suite on `cmake-build-x86jit`, `cmake-build-x86jit-hv32`,
  `cmake-build-x86jit-boxed`, and the handle-san tree
  `cmake-build-hs-hv32` (build target `hermes` in each first). The dump
  tests are UNSUPPORTED under handle_san; the behavioral ones run there.
- Octane spot behavioral run:
  `cmake-build-x86jit/bin/hermes -Xjit=force -Xjit-crash-on-error
  benchmarks/octane/richards.js` and the same with `-Xjit-max-recompiles=0`
  and with plain interpreter; all three outputs must agree.

Expected: all green/identical.

- [ ] **Step 5: Commit**

```bash
git add test/jit/x86-64/recompile-mid-recursion.js test/jit/x86-64/recompile-deterministic.js utils/jit/jit-dump.sh
git commit -m "JIT: recompilation stress tests; pin baseline dumps to first compiles"
```

---

### Task 6: documentation

**Files:**
- Modify: `doc/JIT.md` (new section after "Inline write barrier")
- Modify: `doc/JITTesting.md` (note the `-Xjit-max-recompiles=0`
  baseline convention next to the jit-dump.sh workflow, ~line 134)
- Modify: `doc/superpowers/specs/2026-09-04-jit-recompilation-design.md`
  (append a short "Delivered" section)

**Interfaces:** none (prose only).

- [ ] **Step 1: doc/JIT.md section**

Add a section titled "Recompilation" covering, in this order and
briefly (the spec holds the full rationale — link it):
what triggers a recompile (PutById helper declines +
`kRecompileDeclineThreshold` + warm-cache progress check), what installs
it (`setJITCompiled`, per-call pointer load, old activations finish on
the retired body), the budget flag (`-Xjit-max-recompiles`, default 1,
0 = off), version labels in `-Xdump-jitcode` output, and the explicit
non-goals (no deopt, no OSR, retired code is not freed — dz issue
reference for reclamation).

- [ ] **Step 2: doc/JITTesting.md note**

In the dump-baseline workflow section, document that baselines are
first-compile output and `jit-dump.sh` passes `-Xjit-max-recompiles=0`
for that reason.

- [ ] **Step 3: Spec Delivered section**

Append to the spec:

```markdown
## Delivered (2026-09-XX)

Implemented on x86-64 per this spec; see doc/JIT.md "Recompilation".
Tests: test/jit/x86-64/recompile-*.js. arm64 carries the trivial
JITContext setter only; its emitter does not yet report cold sites, so
the mechanism is dormant there (coldByIdSites stays 0).
```

(fill in the real date).

- [ ] **Step 4: Commit**

```bash
git add doc/JIT.md doc/JITTesting.md doc/superpowers/specs/2026-09-04-jit-recompilation-design.md
git commit -m "JIT: document recompilation"
```

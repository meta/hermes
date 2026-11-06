# JIT Per-Version Feedback Records Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retrofit the recompilation mechanism so each compiled body owns a
self-describing `JitVersionData` record passed to the recording helper in
place of the CodeBlock, making feedback version-attributed.

**Architecture:** `JitVersionData` (back-pointer, body, counter, cold
lists, consumer slot) is allocated per compile, owned by a `Compiler`
member across the longjmp boundary, embedded as the helper-call immediate
by both backends, and installed as `JitFunctionData::current` (prior
current retiring) together with the `setJITCompiled` publication.
`considerRecompile` gains a one-compare staleness gate.

**Tech Stack:** C++17, x86-64 + arm64 JIT backends, lit/FileCheck.

**Spec:** `doc/superpowers/specs/2026-09-07-jit-version-data-design.md`
(externally reviewed; implement exactly its protocol).

## Global Constraints

- Work in `/home/tmikov/work/hermes-x86-jit`, branch `x86-jit`. Never `cd`.
- Primary tree `cmake-build-x86jit`; arm64 cross tree `cmake-build-arm64`
  (qemu configured). Suites: `LIT_OPTS="-j8" LIT_FILTER="jit"
  cmake --build <tree> --target check-hermes`. Current baselines:
  x86 83 passed / 3 unsupported; arm64 53 passed / 33 unsupported.
- 80 columns, 2-space indent, doc comment on every declaration; invoke the
  `gc-safe-coding` skill before lib/VM edits (the trigger stays at helper
  entry, before any raw object pointer — that placement is a GC contract).
- Do NOT modify `_sh_ljs_*` helpers in lib/VM/StaticH.cpp.
- Every commit ends with these trailer lines exactly:
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB
- Line numbers below are anchors, not gospel — anchor on the quoted code.

---

### Task 1: the core retrofit (both backends, one commit)

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h`
- Modify: `lib/VM/JIT/JitHandlers.h`, `lib/VM/JIT/JitHandlers.cpp`
- Modify: `include/hermes/VM/JIT/x86-64/JIT.h`,
  `include/hermes/VM/JIT/arm64/JIT.h` (considerRecompile signature)
- Modify: `lib/VM/JIT/JitCompiler.cpp` (Compiler member, install
  protocol, recompile/considerRecompile)
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h` + `lib/VM/JIT/arm64/JitEmitter.h`
  (Emitter ctor param + member), and the one-line call-setup swap in
  `lib/VM/JIT/x86-64/JitEmitter-property.cpp` (~line 1135) and
  `lib/VM/JIT/arm64/JitEmitter-property.cpp` (~line 1145)

**Interfaces:**
- Produces: `struct JitVersionData { CodeBlock *codeBlock;
  JITCompiledFunctionPtr body; uint32_t declineCount; uint16_t
  coldByIdSites; SmallVector<uint8_t,4> coldWriteCacheIdxs /
  coldReadCacheIdxs; void *consumerRecords; }` with
  `explicit JitVersionData(CodeBlock *)`.
- Produces: `JitFunctionData { kRecompileDeclineThreshold=64;
  recompileBudget; version; unique_ptr<JitVersionData> current;
  SmallVector<unique_ptr<JitVersionData>,1> retired; }`.
- Produces: `_jit_put_by_id(SHRuntime *, SHJitVersionData *, ...)` and
  `JITContext::considerRecompile(Runtime &, JitVersionData *)`.

- [ ] **Step 1: restructure JitFunctionData.h**

Define `JitVersionData` above `JitFunctionData` (add `#include <memory>`;
`class CodeBlock;` forward declaration; the `JITCompiledFunctionPtr`
typedef already exists in this header). Move `declineCount`,
`coldByIdSites`, the two cold-index vectors, and `consumerRecords` into
`JitVersionData`, add `CodeBlock *codeBlock` (set by the constructor) and
`JITCompiledFunctionPtr body = nullptr`. `JitFunctionData` keeps
`kRecompileDeclineThreshold`, `recompileBudget`, `version`, and gains
`std::unique_ptr<JitVersionData> current{}` plus
`llvh::SmallVector<std::unique_ptr<JitVersionData>, 1> retired{}`
(replacing the bare pointer vector). Copy the spec's doc comments; state
on `retired` that a frame may still be executing a retired body and its
record must outlive every possible execution.

- [ ] **Step 2: the helper surface**

`lib/VM/JIT/JitHandlers.h`: add `typedef struct SHJitVersionData
SHJitVersionData;` next to the existing opaque typedefs and change
`_jit_put_by_id`'s second parameter from `SHCodeBlock *codeBlock` to
`SHJitVersionData *versionData` (doc comment: the calling body's version
record; the CodeBlock is reached through it).

`lib/VM/JIT/JitHandlers.cpp`: replace the existing entry trigger IN PLACE
(it must remain the first statement — before `Runtime &runtime`, before
any raw object pointer; recompilation may allocate):

```cpp
  // Recompilation trigger: every call to this helper is a decline of
  // the inline PutById tier of the CALLING BODY, whose version record
  // identifies it. Threshold crossings hand off to the JITContext;
  // retired bodies' events land in their own frozen records and spend
  // nothing. Runs before any raw object pointer is derived:
  // recompilation may allocate.
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  if (LLVM_UNLIKELY(
          ++vd->declineCount >=
          JitFunctionData::kRecompileDeclineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
```

and derive the CodeBlock below it: `CodeBlock *curCodeBlock =
vd->codeBlock;` (replacing the `(CodeBlock *)codeBlock` casts).

- [ ] **Step 3: JITContext signatures (three headers)**

In `include/hermes/VM/JIT/x86-64/JIT.h` and the arm64 mirror: forward
declare `struct JitVersionData;` and change `considerRecompile` to
`(Runtime &runtime, JitVersionData *versionData)`, updating its doc
comment per the spec (staleness gate first). `recompile(Runtime &,
CodeBlock *)` is unchanged. The no-JIT stub in
`include/hermes/VM/JIT/JIT.h` has neither method — verify, and leave it.

- [ ] **Step 4: Compiler ownership and the install protocol**

In `lib/VM/JIT/JitCompiler.cpp`, class `JITContext::Compiler` (~line 51):
declare the owning member BEFORE `em_` (initialization order follows
declaration order, and the emitter receives the raw pointer at
construction):

```cpp
  /// The version record of the body being compiled. Owned here — not as
  /// a local in compileCodeBlockImpl() — because the emitter's error
  /// path leaves compilation by longjmp, which skips local destructors
  /// (see exceptionHandlers_); the Compiler itself sits outside the
  /// jump boundary. Transferred to JitFunctionData::current at install;
  /// destroyed with the Compiler on either failure path.
  std::unique_ptr<JitVersionData> candidateVD_;
```

Initialize it first in the ctor init list
(`candidateVD_(new JitVersionData(codeBlock)),`) and pass
`candidateVD_.get()` as a new `Emitter` constructor argument (Step 5).

Rewrite the install region of `compileCodeBlockImpl()` (currently
`setJITCompiled(em_.addToRuntime(...))` followed by the metadata fill)
to the spec's unified protocol:

```cpp
  JITCompiledFunctionPtr fn = em_.addToRuntime(jc_.impl_->jr);

  // Install: create-or-get the function metadata (budget set on first
  // allocation only), fill the candidate record, retire the previous
  // current, publish. Shared by first compiles and recompiles.
  JitFunctionData *jitData =
      codeBlock_->ensureJitData(jc_.getMaxRecompiles());
  candidateVD_->coldWriteCacheIdxs = std::move(em_.coldWriteCacheIdxs_);
  candidateVD_->coldReadCacheIdxs = std::move(em_.coldReadCacheIdxs_);
  size_t coldTotal = candidateVD_->coldWriteCacheIdxs.size() +
      candidateVD_->coldReadCacheIdxs.size();
  candidateVD_->coldByIdSites =
      coldTotal > 0xffff ? 0xffff : (uint16_t)coldTotal;
  candidateVD_->body = fn;
  if (jitData->current)
    jitData->retired.push_back(std::move(jitData->current));
  jitData->current = std::move(candidateVD_);
  codeBlock_->setJITCompiled(jitData->current->body);
```

Adjust the later dump line to read `jitData->current->coldByIdSites`.
The memory-limit `return nullptr` remains BEFORE `addToRuntime`, so both
failure paths (that return and the longjmp) leave `current`/`retired`
untouched and destroy the candidate with the Compiler. The pre-emission
version local (`priorData ? priorData->version + 1u : 1u`) and its three
uses (banner, success line, perf suffix) are unchanged — JitFunctionData
still first appears at install, so the computation stays valid.

`recompile()` loses its `retired.push_back(old)` (now in the install) and
keeps: the compiled-body precondition, calling `compileImpl`, and on
success `--recompileBudget`, `++version`, the `NumRecompiles` counter
bump; on failure `recompileBudget = 0`.

`considerRecompile(Runtime &runtime, JitVersionData *vd)`:

```cpp
  if (counters_.get())
    ++counters_.get()[(unsigned)JitCounter::NumRecompileChecks];
  vd->declineCount = 0;
  JitFunctionData *data = vd->codeBlock->getJitData();
  // Staleness gate: a retired body's events influence nothing.
  if (vd != data->current.get())
    return;
  if (!enabled_ || !data->recompileBudget || vd->coldByIdSites == 0)
    return;
  ...existing per-site warm scan, reading vd->coldWriteCacheIdxs /
  vd->coldReadCacheIdxs...
  recompile(runtime, vd->codeBlock);
```

- [ ] **Step 5: the emitters (mirrored)**

Both `JitEmitter.h` files: add a constructor parameter
`JitVersionData *versionData` (after `codeBlock`) stored as
`JitVersionData *const versionData_;` with a doc comment (the record of
the body being emitted; embedded as the recording helpers' identity
argument), plus the `struct JitVersionData;` forward declaration.
Update both Emitter constructor definitions and the `Compiler` ctor call.

Both `JitEmitter-property.cpp` files, in the `_jit_put_by_id` call setup,
swap the one materialization line and the typed binding:

```cpp
  loadBits64InGp(x86::rsi, (uint64_t)versionData_, "JitVersionData");
```

(arm64: same on `a64::x1`), and in the `EMIT_RUNTIME_CALL` function type
change `SHCodeBlock *codeBlock` to `SHJitVersionData *versionData` — the
typed binding is what forces both backends to be updated together.

- [ ] **Step 6: build and gates**

- `cmake --build cmake-build-x86jit --target hermes`; full jit filter:
  expect exactly 83 passed / 3 unsupported (the existing recompile tests
  pin threshold semantics for the current version, which are unchanged).
- `cmake --build cmake-build-arm64 --target hermes` (complete binary) and
  its jit filter: expect 53 passed / 33 unsupported.
- If any existing test changes behavior, STOP and report before adapting.

- [ ] **Step 7: commit**

```bash
git add include/hermes/VM/JIT lib/VM/JIT
git commit -m "JIT: per-version feedback records (JitVersionData)"
```

(4-8 line body: what moved, the ownership/longjmp reasoning, the
staleness gate, the one-immediate swap; plus the trailer lines.)

---

### Task 2: the two-phase staleness regression test

**Files:**
- Test: `test/jit/x86-64/recompile-staleness-budget.js`

**Interfaces:** consumes Task 1's landed behavior only.

- [ ] **Step 1: write the test** (Meta copyright header from a sibling;
80 columns):

```js
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-max-recompiles=2 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Two phases pin that a RETIRED body's slow-path declines cannot spend
// the recompile budget (they land in the retired version's own record),
// while the current body's declines still can.
//
// The deep call: descent stores o.p at every level; v1's declines cross
// the threshold (64) mid-descent, p's cache is warm, version 2 installs
// with p's tier and q on its cold list (q has never executed). The
// REMAINING DESCENT runs v2; only the UNWIND executes in the ~65 live
// v1 frames. The unwind stores o.q at every level: ~37 declines from v2
// frames, ~64 from retired v1 frames, and the helper calls warm q's
// cache. Under shared counting the v1 declines would cross the
// threshold with q warmed and produce version 3 DURING the unwind;
// per-version counters must not (phase 1 pin). Phase 2's shallow calls
// each decline once on q under v2, crossing v2's own threshold at ~27
// calls: version 3 must appear, with q's tier — proving the budget
// survived phase 1 and triggering still works.

function rec(o, n) {
  o.p = n;
  if (n > 0)
    rec(o, n - 1);
  o.q = n;
  return o.p;
}

var o = {p: -1, q: -1};
print(rec(o, 100));
print('phase two');
for (var i = 0; i < 40; ++i)
  rec(o, 0);
print(o.q);

// OUT: 0
// OUT-NEXT: phase two
// OUT-NEXT: 0

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec'
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 2)
// DUMP-NOT: 'rec' (version 3)
// DUMP: phase two
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'rec' (version 3)
// DUMP: 0
```

Verify the actual dump ordering by hand first and adjust to what really
prints (in particular whether the v2 banner precedes the first program
output) — but the essentials are fixed: no version 3 before `phase two`,
version 3 required after it, both value outputs pinned.

- [ ] **Step 2: prove the phase-1 pin can fail**

Temporarily disable the staleness gate in `considerRecompile` (comment
out the `vd != data->current.get()` early return), rebuild, run the DUMP
FileCheck: it must FAIL with version 3 appearing before `phase two`
(the old shared-counting behavior reproduced). Restore, rebuild, confirm
pass. Capture both outputs in your report.

- [ ] **Step 3: wider gates**

Full jit filter on `cmake-build-x86jit` (expect 84/3), and build +
jit filter on `cmake-build-x86jit-hv32` and `cmake-build-x86jit-boxed`
(expect 84/3 each; heap mode does not affect the mechanism).

- [ ] **Step 4: commit**

```bash
git add test/jit/x86-64/recompile-staleness-budget.js
git commit -m "JIT: pin that retired-version declines cannot spend recompile budget"
```

(plus trailers.)

---

### Task 3: documentation

**Files:**
- Modify: `doc/JIT.md` (Recompilation section)
- Modify: `doc/superpowers/specs/2026-09-07-jit-version-data-design.md`
  (append Delivered section)
- Modify: `doc/superpowers/specs/2026-09-04-jit-recompilation-design.md`
  (one-line pointer under its metadata section: superseded in part by
  the 2026-09-07 spec)

- [ ] **Step 1:** update doc/JIT.md's Recompilation section: the helper
receives the calling body's `JitVersionData` (the CodeBlock reached
through it); declines are counted per version; retired bodies' events
land in frozen records and spend nothing; metadata split (control state
per function, observations per version). Verify every sentence against
the landed code; 80 columns; match voice.

- [ ] **Step 2:** append to the 09-07 spec:

```markdown
## Delivered (YYYY-MM-DD)

Implemented as specified; see doc/JIT.md "Recompilation" and
test/jit/x86-64/recompile-staleness-budget.js (the two-phase pin).
Both backends pass the record; arm64 remains dormant (empty cold
lists).
```

(real date), and add the pointer line to the 09-04 spec.

- [ ] **Step 3:** jit filter once on `cmake-build-x86jit` (the standard
end-of-task gate), then commit:

```bash
git add doc
git commit -m "JIT: document per-version feedback records"
```

(plus trailers.)

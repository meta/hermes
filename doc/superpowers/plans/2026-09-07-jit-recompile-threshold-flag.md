# Configurable JIT Recompile Decline Threshold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `JitFunctionData::kRecompileDeclineThreshold` (64) from a
compile-time constant into a runtime knob, `-Xjit-recompile-threshold`,
so the recompile trigger's eagerness can be tuned without rebuilding.

**Architecture:** The threshold plumbs exactly like
`-Xjit-max-recompiles` (RuntimeFlags opt -> RuntimeConfig field -> the
two builder chains -> `Runtime.cpp` -> a `JITContext` member), and is
then **snapshotted into `JitVersionData` at that version's compile**,
next to `declineCount`. The helper tail compares two fields of the one
record it already holds; it never dereferences the `CodeBlock`, the
`JitFunctionData`, or the `JITContext` on the counting path.

**Tech Stack:** C++17, x86-64 + arm64 JIT backends, lit/FileCheck.

**Issue:** dz `01a077ac` (component `jit`). Closed by this change.

## Design decision (settled; do not relitigate)

The trigger reads the threshold from **`JitVersionData`**, snapshotted at
the version's compile — the issue's option (a), but on `JitVersionData`,
not `JitFunctionData`.

Why not `JitFunctionData`: the issue's parenthetical assumes the helper
tail already touches `JitFunctionData` and would keep "touching a single
cache line". It does not. The landed tail is

```cpp
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  if (LLVM_UNLIKELY(
          ++vd->declineCount >= JitFunctionData::kRecompileDeclineThreshold)) {
```

— one structure, `JitVersionData`, reached directly from the helper's own
argument. Snapshotting into `JitFunctionData` would cost the tail
`vd->codeBlock` -> `jitData_` (a `unique_ptr` load) -> the field: three
extra dependent loads on the path taken by *every* decline, to reach data
that never changes. Snapshotting into `JitVersionData` puts the threshold
in the same cache line as the counter the tail already writes, so the
compare stays register-cheap and the tail's memory traffic is unchanged.
Option (b) (read the `JITContext` each time) adds a live `Runtime` deref
to the same hot path for a value that is set once at Runtime
construction, and option (c) calls into `considerRecompile` on every
decline rather than once per crossing — the trigger's whole point is that
it is cheap between crossings. The one thing option (a) gives up — a
mid-run setter change not affecting already-compiled bodies — costs
nothing: the only caller of the setter is `Runtime`'s constructor, and a
version compiled after a change picks the new value up anyway.

**Width:** `uint32_t` throughout (`declineCount` is `uint32_t`, the
`cl::opt` is `uint32_t`, the `RuntimeConfig` field is `uint32_t`). No
clamping to 8 or 16 bits: unlike `maxRecompiles` (a `uint8_t` member,
hence the `std::min(...,255)` in Runtime.cpp), nothing downstream is
narrower. The extra 4 bytes land in `JitVersionData`'s existing tail
padding region — one per compiled version, against two `SmallVector`s
already in the struct.

**Value 0:** clamped to 1 in `Runtime.cpp`. `0` needs no "off" meaning —
`-Xjit-max-recompiles=0` is already the off switch — and left unclamped
`++vd->declineCount >= 0` is trivially true, i.e. exactly the option (c)
behaviour we rejected, reached by accident. Clamping to 1 makes the flag
monotone over its whole domain (smaller = earlier, floor at "every
decline") with no special case for the reader to remember.

## Global Constraints

- Work in `/home/tmikov/work/hermes-x86-jit`, branch `x86-jit`. Never `cd`.
- Primary tree `cmake-build-x86jit`; arm64 cross tree `cmake-build-arm64`
  (qemu configured). Suite: `LIT_OPTS="-j8" LIT_FILTER="jit"
  cmake --build <tree> --target check-hermes`. **Measured baselines
  before this change:** x86 84 passed / 3 unsupported; arm64 53 passed /
  33 unsupported.
- 80 columns, 2-space indent, doc comment on every declaration. Invoke
  the `gc-safe-coding` skill before the lib/VM edits; the trigger must
  stay at helper entry, before any raw object pointer is derived (that
  placement is a GC contract, and this change must not move it).
- One commit for the whole change: the flag is untested without the
  tests, and the existing tests are wrong without the flag.
- Line numbers below are anchors, not gospel — anchor on the quoted code.

---

### Task 1: the flag, the tests that pin it, and the doc (one commit)

**Files:**
- Modify: `include/hermes/VM/JIT/JitFunctionData.h`
- Modify: `lib/VM/JIT/JitHandlers.cpp`, `lib/VM/JIT/JitCompiler.cpp`
- Modify: `include/hermes/VM/JIT/x86-64/JIT.h`,
  `include/hermes/VM/JIT/arm64/JIT.h`, `include/hermes/VM/JIT/JIT.h`
- Modify: `include/hermes/VM/RuntimeFlags.h`,
  `public/hermes/Public/RuntimeConfig.h`
- Modify: `tools/hermes/hermes.cpp`, `tools/test-runner/Executor.cpp`
- Modify: `lib/VM/Runtime.cpp`
- Modify: `doc/JIT.md`
- Modify: 7 lit tests under `test/jit/x86-64/` (RUN lines)
- Create: `test/jit/x86-64/recompile-threshold-flag.js`

**Interfaces:**
- Produces: `-Xjit-recompile-threshold=N` (default 64, `0` means 1).
- Produces: `RuntimeConfig::getJITRecompileThreshold()` /
  `Builder::withJITRecompileThreshold()`.
- Produces: `JITContext::setRecompileDeclineThreshold(uint32_t)` /
  `getRecompileDeclineThreshold()` on both backends and the stub.
- Produces: `JitVersionData::declineThreshold` and
  `JitVersionData(CodeBlock *, uint32_t)`.
- Renames: `JitFunctionData::kRecompileDeclineThreshold` ->
  `kDefaultRecompileDeclineThreshold` (it is now only a default).

- [ ] **Step 1: `JitVersionData` carries the threshold**

In `include/hermes/VM/JIT/JitFunctionData.h`, insert the new field
directly after `declineCount` (adjacency is the point — see the design
decision) and give the constructor its second parameter:

```cpp
  /// This body's own helper-side decline counter. A retired body keeps
  /// counting into its own frozen record, where it influences nothing.
  uint32_t declineCount = 0;
  /// Declines this body must accumulate before its helper tail hands
  /// off to JITContext::considerRecompile. Snapshotted from the
  /// JITContext (i.e. from -Xjit-recompile-threshold) when this version
  /// is compiled, so the tail compares two fields of the one record it
  /// already holds and never walks to the CodeBlock or the JITContext.
  /// Never 0: the setter's caller clamps it up to 1.
  uint32_t declineThreshold;
```

and

```cpp
  /// \param cb the function this body is compiled from.
  /// \param declineThreshold declines before considering a recompile.
  JitVersionData(CodeBlock *cb, uint32_t declineThreshold)
      : codeBlock(cb), declineThreshold(declineThreshold) {}
```

(the constructor loses `explicit`, which was there only because it was a
one-argument constructor).

In the same header, rename the constant and restate it as a default:

```cpp
  /// Default declines of the ById helpers before the recompile check
  /// runs; the effective value is per-version and lives in
  /// JitVersionData::declineThreshold. Duplicated as a literal by the
  /// -Xjit-recompile-threshold flag and the RuntimeConfig field, which
  /// cannot include this header.
  static constexpr uint32_t kDefaultRecompileDeclineThreshold = 64;
```

- [ ] **Step 2: the trigger tail**

In `lib/VM/JIT/JitHandlers.cpp` (`_jit_put_by_id`, ~line 328), the
comparison becomes record-local. Leave the surrounding comment and the
statement's position at helper entry untouched:

```cpp
  JitVersionData *vd = reinterpret_cast<JitVersionData *>(versionData);
  if (LLVM_UNLIKELY(++vd->declineCount >= vd->declineThreshold)) {
    getRuntime(shr).getJITContext().considerRecompile(getRuntime(shr), vd);
  }
```

- [ ] **Step 3: snapshot at compile**

In `lib/VM/JIT/JitCompiler.cpp`, the `Compiler` constructor's member
initializer (~line 103):

```cpp
        candidateVD_(
            new JitVersionData(codeBlock, jc.getRecompileDeclineThreshold())),
```

- [ ] **Step 4: the JITContext knob (both backends + stub)**

In `include/hermes/VM/JIT/x86-64/JIT.h` and
`include/hermes/VM/JIT/arm64/JIT.h`, add the include that makes the
default constant reachable (both currently only forward-declare
`JitVersionData`; `CodeBlock.h` forward-declares `JitFunctionData` and
does not include this header):

```cpp
#include "hermes/VM/JIT/JitFunctionData.h"
```

immediately after `#include "hermes/VM/JIT/JitCounters.h"`.

Add the accessors right after `getMaxRecompiles()` in each:

```cpp
  /// Set the number of ById helper declines within one compiled body
  /// before a recompile is considered. Applies to versions compiled
  /// from here on; each version snapshots it at its own compile.
  /// \pre threshold >= 1.
  void setRecompileDeclineThreshold(uint32_t threshold) {
    assert(threshold >= 1 && "recompile decline threshold must be >= 1");
    recompileDeclineThreshold_ = threshold;
  }

  /// \return the declines before a recompile is considered.
  uint32_t getRecompileDeclineThreshold() const {
    return recompileDeclineThreshold_;
  }
```

and the member right after `maxRecompiles_` in each:

```cpp
  /// Declines within one compiled body before a recompile is considered.
  uint32_t recompileDeclineThreshold_{
      JitFunctionData::kDefaultRecompileDeclineThreshold};
```

If `<cassert>` is not already reachable in either header, use
`hermes/Support/*` conventions already present in the file rather than
adding an include; if in doubt drop the assert and keep the doc comment's
`\pre`.

While in these two headers, fix the now-stale `considerRecompile` doc
comment (~line 191 arm64 / ~194 x86-64):

```cpp
  /// Called by JIT runtime helpers when the decline counter of the body
  /// they were called from reaches that body's own
  /// JitVersionData::declineThreshold (from -Xjit-recompile-threshold).
```

In the no-JIT stub `include/hermes/VM/JIT/JIT.h`, mirror both accessors
as no-ops next to `setMaxRecompiles`/`getMaxRecompiles`:

```cpp
  /// Set the ById declines within one body before considering a
  /// recompile (no-op without the JIT).
  void setRecompileDeclineThreshold(uint32_t threshold) {}

  /// \return the declines before a recompile is considered.
  uint32_t getRecompileDeclineThreshold() const {
    return 0;
  }
```

- [ ] **Step 5: the flag and the config field**

`include/hermes/VM/RuntimeFlags.h`, directly after the `JITMaxRecompiles`
opt (~line 250):

```cpp
  llvh::cl::opt<uint32_t> JITRecompileThreshold{
      "Xjit-recompile-threshold",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("ById helper declines within one compiled body "
                     "before a recompile is considered (0 means 1)"),
      llvh::cl::init(64)};
```

`public/hermes/Public/RuntimeConfig.h`, directly after the
`JITMaxRecompiles` entry (~line 144). The continuation backslashes sit in
column 72 — these lines are already padded to match:

```
  /* ById declines in one body before a recompile is considered.    */ \
  /* 0 is clamped to 1. Mirrors kDefaultRecompileDeclineThreshold.  */ \
  F(constexpr, uint32_t, JITRecompileThreshold, 64)                    \
                                                                       \
```

(Copy these lines verbatim, then eyeball the backslash column against the
neighbouring entries before building — a misaligned backslash still
compiles, so nothing will catch it for you.)

- [ ] **Step 6: both builder chains**

`tools/hermes/hermes.cpp` (~line 120), after `.withJITMaxRecompiles(...)`:

```cpp
          .withJITRecompileThreshold(flags.JITRecompileThreshold)
```

`tools/test-runner/Executor.cpp` (~line 221) — the review on the previous
change caught this file being skipped; do not skip it. The chain ends
with a `;`, so move it:

```cpp
          .withJITMaxRecompiles(config.runtimeFlags->JITMaxRecompiles)
          .withJITRecompileThreshold(
              config.runtimeFlags->JITRecompileThreshold);
```

- [ ] **Step 7: apply it in Runtime**

`lib/VM/Runtime.cpp` (~line 489), after the `setMaxRecompiles` call. This
is where `0` becomes `1` (see the design decision); `std::min` is already
used here, so `<algorithm>` is reachable:

```cpp
  jitContext_.setRecompileDeclineThreshold(
      std::max<uint32_t>(runtimeConfig.getJITRecompileThreshold(), 1));
```

- [ ] **Step 8: build and check the flag parses**

```bash
cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-x86jit --target hermes
/home/tmikov/work/hermes-x86-jit/cmake-build-x86jit/bin/hermes \
  --help-hidden 2>&1 | grep -A2 Xjit-recompile-threshold
echo 'var o={p:0}; function f(o,v){o.p=v;} for(var i=0;i<20;++i) f(o,i); print(o.p);' > /tmp/thr.js
/home/tmikov/work/hermes-x86-jit/cmake-build-x86jit/bin/hermes \
  -Xjit=force -Xjit-recompile-threshold=8 -Xdump-jitcode=2 /tmp/thr.js | grep "version 2"
/home/tmikov/work/hermes-x86-jit/cmake-build-x86jit/bin/hermes \
  -Xjit=force -Xjit-recompile-threshold=0 /tmp/thr.js
```

The `--help-hidden` line must show the flag and its description; the
`=8` run must print at least one `(version 2)` banner; the `=0` run must
print `19` and not assert (proof the clamp works). Record all three
outputs in your report.

- [ ] **Step 9: pin the existing tests at 64**

Seven tests bake the default into their arithmetic and would fail loudly,
not silently, if the default ever moved. Add
`-Xjit-recompile-threshold=64` to each `%hermes` RUN line below, placed
immediately after the last existing `-Xjit-*` flag on that line. **The
issue names five; two more (`recompile-deterministic.js`,
`recompile-switch-old-frames.js`) depend on a version-2 body appearing at
the default just as directly — they are included here.** Not touched:
`recompile-cold-sites.js` (20 iterations, no version-2 dependency, no
version `-NOT`).

1. `recompile-byid-warm.js` lines 8, 9, 10 (all three).
2. `recompile-phased-warming.js` line 8.
3. `recompile-mid-recursion.js` line 8.
4. `recompile-staleness-budget.js` lines 8, 9.
5. `recompile-hcid-exhausted.js` lines 8, 9.
6. `recompile-deterministic.js` lines 8, 9 only (lines 10-11 are `grep`
   and `diff`, not `%hermes`).
7. `recompile-switch-old-frames.js` lines 8, 9, 11 only (line 10 is a
   plain non-JIT `%hermes` run; leave it alone).

Also update the prose in the three tests that name the number, so the
comment points at the RUN line rather than at a constant: in
`recompile-byid-warm.js` "past the threshold (64)", in
`recompile-mid-recursion.js` "The decline threshold (64)", and in
`recompile-staleness-budget.js` "v1's declines cross the threshold (64)",
change "(64)" to "(64, pinned on the RUN line)".

- [ ] **Step 10: the new test**

Create `test/jit/x86-64/recompile-threshold-flag.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=8 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=EARLY %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xdump-jitcode=2 %s | %FileCheck --check-prefix=LATE %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// -Xjit-recompile-threshold sets how many declines of one compiled body
// are needed before a recompile is considered. Both runs below are the
// same program: under -Xjit=force, f compiles before it ever runs, its
// write cache is cold, the PutById inline tier is not emitted, and every
// one of the 20 calls declines into the helper. At =8 the eighth decline
// crosses and f gets a version 2 (its cache warmed on call one, so the
// progress check passes); at the default 64, twenty declines never
// cross and f stays on version 1.
//
// The version checks name 'f' explicitly: 'global' has ById sites of its
// own (the var accesses in the loop) and may well recompile in either
// run, which says nothing about f.

function f(o, v) {
  o.p = v;
}

var o = {p: 0};
for (var i = 0; i < 20; ++i)
  f(o, i);
print(o.p);

// EARLY: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// EARLY: JIT compilation of FunctionID {{[0-9]+}}, 'f' (version 2)
// EARLY: 19

// The -NOT is bounded to the window between f's first successful compile
// and the program's only output: that is the whole interval in which a
// version-2 banner for f could be printed at all.
// LATE: JIT successfully compiled FunctionID {{[0-9]+}}, 'f'
// LATE-NOT: 'f' (version
// LATE: 19
```

Run the two commands by hand first and read the real dump before
trusting the ordering above; adjust the CHECK lines to what actually
prints. The essentials are fixed and must not be weakened: a version-2
banner **for f** required at `=8`, forbidden at `=64`, same source, same
flags otherwise.

- [ ] **Step 11: prove the new test can fail**

Not by editing the RUN line — that only proves FileCheck works. Break the
plumbing: in `lib/VM/JIT/JitCompiler.cpp` (Step 3), temporarily pass
`JitFunctionData::kDefaultRecompileDeclineThreshold` instead of
`jc.getRecompileDeclineThreshold()`, rebuild `hermes`, and run only the
new test:

```bash
LIT_OPTS="-j8" LIT_FILTER="recompile-threshold-flag" \
  cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-x86jit \
  --target check-hermes
```

It must **FAIL**, on the EARLY prefix, with no version-2 banner for `f` —
i.e. the flag reaching the trigger is exactly what the test measures.
Restore the line, rebuild, confirm it passes. Capture both runs' output
in your report; a claim without the failing output does not count.

- [ ] **Step 12: doc/JIT.md**

Two edits in the Recompilation section. At ~line 658, replace
"At `JitFunctionData::kRecompileDeclineThreshold` (64) it hands off to"
with:

```
its back-pointer. At that body's own decline threshold -- snapshotted
into its `JitVersionData` when it was compiled, default 64, set by
`-Xjit-recompile-threshold` -- it hands off to
```

and extend the **Budget.** paragraph at ~line 694:

```
**Budget.** `-Xjit-max-recompiles=N` sets the per-function recompile
budget; default 1, and `0` restores the exact compile-once behavior
this mechanism extends. `-Xjit-recompile-threshold=N` sets how many
declines of one compiled body precede a recompile check; default 64,
and `0` is clamped to 1 (there is no "off" value -- that is what
`-Xjit-max-recompiles=0` is for). It applies to bodies compiled after
it is set, which in practice means all of them.
```

Also add `recompile-threshold-flag.js` to the test list at ~line 717.
Verify every sentence against the landed code; 80 columns; match voice.

- [ ] **Step 13: gates**

```bash
LIT_OPTS="-j8" LIT_FILTER="jit" \
  cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-x86jit \
  --target check-hermes
```
Expect **85 passed / 3 unsupported** (baseline 84/3, one test added).

```bash
LIT_OPTS="-j8" LIT_FILTER="jit" \
  cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-arm64 \
  --target check-hermes
```
Expect **53 passed / 34 unsupported** (baseline 53/33; the new test is
under `test/jit/x86-64/`, which `lit.local.cfg` marks unsupported
without the x86-64 backend). The arm64 tree is the real gate for the
`JIT.h` mirror and the shared `JitHandlers.cpp` tail — it must build
clean, no warnings.

Also build the no-JIT path is covered by the stub edit: confirm
`cmake-build-host` still builds (`--target hermes`), since that is the
tree where `include/hermes/VM/JIT/JIT.h`'s stub is compiled.

- [ ] **Step 14: close the issue and commit**

```bash
dz close 01a077ac --as fixed
git add -A include lib tools public test/jit/x86-64 doc dz
git commit
```

Commit message:

```
JIT: make the recompile decline threshold configurable

Adds -Xjit-recompile-threshold (default 64), plumbed like
-Xjit-max-recompiles: RuntimeFlags opt, RuntimeConfig field, both
builder chains, Runtime.cpp, and a JITContext member on both backends
and the no-JIT stub. Each compiled version snapshots the value into its
own JitVersionData at compile time, so the trigger tail still compares
two fields of the record it already holds and never walks to the
CodeBlock or the JITContext. 0 is clamped to 1; the off switch remains
-Xjit-max-recompiles=0.

The seven existing recompile tests bake the default into their decline
arithmetic, so they now pin -Xjit-recompile-threshold=64 explicitly
rather than inheriting it. recompile-threshold-flag.js pins the flag
itself: the same program recompiles f at =8 and does not at =64.

Closes dz 01a077ac.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB
```

Verify with `git show --stat` that the dz issue file and all 8 test files
are in the commit; nothing else should be.

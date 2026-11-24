# JIT Inline Boxed-Double Store Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Under `HERMESVM_BOXED_DOUBLES` (HV32 and BOXED), the PutByVal
JSArray tier and the PutById tier store a double that needs a
`BoxedDouble` without leaving JIT code: they bump-allocate the box in the
young generation, write the double into it and store the tagged pointer,
on x86-64 and arm64, with the HV64 emitted code unchanged.

**Architecture:** The encode moves from the top of each tier into
`emitSafeStoreOrSlow`, after its declining checks. The encoder gains a
`boxLab`; `emitSafeStoreOrSlow` binds it and emits the box arm
(`allocInYoung`, double store, compressed-pointer encode, store,
unconditional card for an old target). A full young generation declines to
the helper through an out-of-line counter block. Three emitted counters
measure it. HV64 compiles none of it.

**Tech Stack:** C++17, asmjit x86-64 and a64, lit/FileCheck (arm64 under
qemu-user).

**Spec:** doc/superpowers/specs/2026-10-29-jit-inline-boxed-double-store-design.md

Read the spec in full before Task 1, and read `DESIGN-PRINCIPLES.md`
(repo root). Where this plan resolves an ambiguity in the spec it says so
in the task ("Spec resolution"); the list is also in the last section.

## Global Constraints

- Scope: the PutByVal JSArray tier (`emitPutByValFastArrayTier`) and the
  PutById tier (`emitPutByIdInlineTier`), through `emitSafeStoreOrSlow`,
  on x86-64 and arm64, under `HERMESVM_BOXED_DOUBLES`. Non-goals (spec
  "Non-goals"): stores declined for old-generation marking or compaction
  (they stay helper calls), object literals with a non-inline double,
  `HERMESVM_SANITIZE_HANDLES` (the encoder's Handle-San arm keeps declining
  every number to `slowLab`), allocation instrumentation, a per-site
  decline reason, typed arrays, `FastArrayStore`/`FastArrayPush`,
  environment slots.
- HV64 (`HEAP_HV_64`): every edit to code HV64 compiles is confined to
  `#ifdef HERMESVM_BOXED_DOUBLES` / `#ifndef` arms; the HV64 emitted code of
  both tiers is byte-identical (spec criterion 2): raw dumps with ASLR
  off, equal line for line except helper addresses that resolve through
  `nm` to the same symbol in both binaries; checked in Tasks 1, 2 and 4.
- Under `HERMESVM_BOXED_DOUBLES` the predicate is exactly:
  ```cpp
  void emitSafeStoreOrSlow(
      const x86::Gp &loc,          // a64::GpX on arm64
      const x86::Gp &value,
      const x86::Gp &t1,
      const x86::Gp &t2,
      const x86::Gp &t3,
      JitCounter boxCounter,
      const asmjit::Label &slowLab);
  ```
  and in the default mode it keeps today's
  `(loc, shv, value, t1, t2, slowLab)`, with the tiers passing `value` as
  `shv`.
- The encoder is exactly
  `emit_shv_encode_or_slow(a, out, in, temp, slowLab, boxLab)`; a double
  that needs a box jumps to `boxLab`, everything else it declines today
  still jumps to `slowLab`. `emit_shv_encode_for_slot_or_slow` is deleted
  from both backends.
- New counters, appended to `JIT_COUNTERS` after `NumByIdStringHits` in
  this order: `NumPutByValBoxes`, `NumPutByIdBoxes`,
  `NumStoreBoxFallbacks`. Emitted only under `-Xjit-emit-counters`;
  `NumPutBy*Boxes` incremented after the box arm's store,
  `NumStoreBoxFallbacks` in the out-of-line `slowFull` block. Without
  counters `slowFull` is `slowLab` itself.
- Emitted comments: `// Box the double in the young generation` at the
  box arm, `// Young generation full: count the fallback` at `slowFull`.
- Every decline precedes the publication of a successful box allocation
  and the slot store: the structural guards and the barrier checks precede
  the encode, the RawHV32 (and Handle-San) rejection is inside the encode,
  and the young-generation-full fallback branches from the bump before the
  new level is stored (spec "Store sequence").
- Every C++ change under `lib/VM/` or `include/hermes/VM/` is written after
  invoking the `gc-safe-coding` skill.
- 80 columns, 2-space indent, a doc comment on every declaration. Before
  each commit format only the changed lines:
  `git clang-format HEAD -- <the task's C++ files>`.
- Never `cd`; use absolute paths, `git -C`, or `( cd dir; cmd; )`. The
  working directory is the repo root, `/home/tmikov/work/hermes-x86-jit`;
  commands below are relative to it.
- Never push or fetch. Never `git add -A`, `git add .` or
  `git add dz/issues`: stage exact paths. Never touch the untracked
  `2026-09-08-putbyval-typed-array-measurements.md` and
  `dz/issues/01a063df-0527-71dc-89c2-2464bff4a1e4.md`.
- Build trees (configured): `cmake-build-x86jit` (ASan Debug, HV64, the
  reference tree), `cmake-build-x86jit-hv32` (`HEAP_HV_PREFER32`),
  `cmake-build-x86jit-boxed` (`HEAP_HV_BOXED`) -- all three
  `HERMESVM_ALLOW_JIT=2`, Hades, `HERMES_SLOW_DEBUG=ON`; Release benchmark
  trees `cmake-build-jit2` (HV64) and `cmake-build-jit2-hv32`; arm64 cross
  trees `cmake-build-arm64`, `cmake-build-arm64-hv32`,
  `cmake-build-arm64-boxed` (Debug, `HERMES_SLOW_DEBUG=ON`, no ASan, run
  under qemu-user, aarch64/README.md); `cmake-build-host` (host compilers
  for the arm64 trees).
- Before building ANY arm64 tree, rebuild the host compilers:
  `cmake --build cmake-build-host --target hermesc shermes`.
- Lit: `cmake --build <tree> --target check-hermes` (full suite: lit and
  unit tests); a subset with `LIT_FILTER='<regex>'` in front, e.g.
  `LIT_FILTER='jit/'` for the JIT suite, `LIT_FILTER='store-boxed-double'`
  for this plan's new tests, `LIT_FILTER='inline-emitted'` for the pins.
  Rebuild the tree (`--target hermes`) before running its suite, and after
  every mutation and every restore.
- arm64 sanity: `./aarch64/qemu-sanity.sh <tree>` with the tree name
  RELATIVE to the repo root (the script builds `$ROOT/$BUILD/bin/hermes`,
  aarch64/qemu-sanity.sh:15-18). Running an arm64 binary by hand:
  `qemu-aarch64-static -L /usr/aarch64-linux-gnu <tree>/bin/hermes ...`.
- A missing card dirty fails loudly only under `HERMES_SLOW_DEBUG`
  (`HadesGC::verifyCardTable()`, lib/VM/gcs/HadesGC.cpp:2681-2685), which
  the Debug trees have; a Release tree passing is not evidence.
- Prove-can-fail: every new test is shown to FAIL with the code it pins
  disabled (the mutation named in the task), then the code is restored, the
  tree REBUILT, and the test re-run green.
- Measurements only from precompiled bytecode: each build's own
  `hermesc -O -emit-binary`.
- `S=/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad`;
  every temporary file goes under `$S/hv32impl`, never in the repo.
- Commits: one per task; fix rounds are folded into the task's commit
  (`git commit --amend`), never separate commits. Message per the
  tightening-commit-messages skill, ending with exactly:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MxvtDWnnS16sfyuVjoNLwC
  ```
- Reporting: each task's report lists the commit hash, the trees built,
  every suite run with its pass/unsupported/fail counts, each mutation with
  the failure it produced, and every command whose output a later task
  quotes. Task 4 copies these into "Delivered".
- The spec and this plan are committed before Task 1. Before Task 1,
  record `git rev-parse HEAD` as BASE in Task 1's report.

## Review Focus

Inputs and conditions the spec implies but its test list does not name,
most likely failure first, with the behavior a person would expect; each
has a test in the owning task:

1. Doubles at the edges of the encoding: both signs, subnormals
   (`5e-324`, bits `0x1`), the largest finite double, `2^28 - 1` (boxes
   under HV32 only), `2^53 - 1`, `1 + 2^-52`, and values that must stay
   inline (`-0`, `NaN`, `Infinity`, `0.5`). Every one must read back
   bit-exact (`Object.is`) through both tiers, into young and old targets,
   before and after a collection. Task 1,
   `test/jit/store-boxed-double-values.js` (interpreter diff).
2. A PutById box stored into an out-of-line (`propStorage`) slot of an old
   object: the card dirtied must be the one covering the slot inside the
   `propStorage` cell, not the object's. Task 1,
   `test/jit/store-boxed-double-card.js`, case `byidwide`.
3. A duo PutByVal site (typed-array tier ahead of the JSArray tier, the
   x86-64 tier's highest register pressure) storing a boxing double into a
   JSArray: version 2 must still emit and run the box arm. Task 1,
   `store-boxed-double-values.js`, `storeDuo` (DUMP lines and diff).
4. An old target while old-generation marking or a compaction is active:
   the store must decline before anything is allocated or stored. Not
   reachable deterministically from JavaScript; pinned by emitted order:
   in `putbyid-inline-emitted` (both backends) the marking, compactee and
   segment-size exits precede the join label, and the box arm follows the
   encode. Tasks 1 and 2.
5. A full young generation without `-Xjit-emit-counters`, where `slowFull`
   is `slowLab` itself: the store must still complete through the helper.
   Task 1, `store-boxed-double-ygfull.js`, second RUN line.

---

## File Structure

- `include/hermes/VM/JIT/JitCounters.h` -- the three counters (Task 1).
- `lib/VM/JIT/x86-64/JitEmitter-internal.h`, `JitEmitter-internal.cpp`,
  `JitEmitter.h`, `JitEmitter-property.cpp` -- encoder `boxLab`, the box
  arm in `emitSafeStoreOrSlow`, its declaration and docs, the tiers' `t3`
  and call sites, wrapper deletion, stale comments (Task 1).
- `lib/VM/JIT/arm64/` -- the same four files (Task 2), plus the ASan
  precondition on the arm64 `bumpAllocAndUnpoison`/`allocInYoung`
  declarations.
- `test/jit/store-boxed-double-{byval,byid,card,ygfull,values}.js` (new,
  Task 1; arm64 gate lifted in Task 2).
- `test/jit/x86-64/put{byval,byid}-inline-emitted.js` (Task 1),
  `test/jit/put{byval,byid}-inline-emitted-arm64.js` (Task 2).
- `doc/JIT.md`, `test/jit/inline-store-shv-shapes.js` comments (Task 3).
- The spec's "Delivered" section, dz `01a185d0-5d43`, dz `01a04e00-0811`,
  a new dz issue for the marking-decline residue (Task 4).

---

### Test conventions (every new lit test)

- Starts with the JS copyright header of test/jit/count.js:1-6.
- Wraps the scenario in `main()`, passes the functions under test and
  `gc` as parameters, and keeps every store site under test alone in its
  own top-level function (DESIGN-PRINCIPLES 6: global reads inside
  functions are GetById sites).
- Counters go to stderr after `JIT counters:`
  (lib/ConsoleHost/ConsoleHost.cpp:1132), one `Name: value` line each in
  `JIT_COUNTERS` order (lib/VM/JIT/x86-64/JIT.cpp:37-44), so counter RUN
  lines use `2>&1`; the three new counters are consecutive, so
  `CHECK-NEXT` chains them.
- Per-mode expectations use `--check-prefixes=CHECK,CHECK-%hv-mode`
  (test/lit.cfg:98-122): `HV64`, `HV32`, `BOXED`.
- A script argument selects a case (`-Xhermes-internal-test-methods %s --
  <arg>`, read with `hermescli.getScriptArgs()[0]`, as
  test/jit/recompile-getidx-demote-carried.js:8-9 and :77 do).
- Compile timing: with `-Xjit -Xjit-threshold=T`, a function of loop depth
  d has threshold `T >> 2d` (include/hermes/VM/JIT/x86-64/JIT.h:289-292,
  arm64/JIT.h:281-284), checked against the execution count before the
  interpreter increments it (lib/VM/Interpreter.cpp:1280, :653). So with
  `T = 4` a loop-free function runs interpreted for its first four calls
  and compiled from the fifth; a function with a loop is compiled from its
  second call; `main`, called once, stays interpreted. Checked by hand on
  this tree: four warm-up calls compile at the first measured call, five
  compile before it.
- Boxing values: `1 + (2k + 1) * Number.EPSILON` for integer
  `0 <= k < 2^51`. They are exact doubles whose lowest mantissa bit is set,
  so the low 35 bits (HV32) and the low 3 bits (BOXED) are nonzero
  (include/hermes/VM/SmallHermesValue.h:323-337), and distinct for distinct
  `k` (spec "Testing").
- GC conditions (spec "Testing", setup shared by the counting tests): no
  test passes a GC flag, makes an external-memory object, or allocates
  more than a few hundred KB before its measured stores. The exact counter
  expectations are themselves the direct evidence for the measured stores:
  an old-target store made while marking is on or a compactee exists
  declines before the box arm (the predicate's checks precede the join),
  so the box counter would fall short of N (of 1 in the card test) and the
  test would fail. A violated condition can only cause a false failure,
  never a masked pass. Tasks 1 and 2 also check the conditions themselves
  on every tree, outside the RUN lines, with
  `-gc-print-collection-stats` and `$S/hv32impl/gc-check.py` (Task 1
  Step 17): every old-generation collection started before the measured
  region, completed or not, and the peak old-generation allocated plus
  external bytes.

---

### Task 1: x86-64 inline boxing, counters and tests

**Files:**
- Modify: `include/hermes/VM/JIT/JitCounters.h:44-46`
- Modify: `lib/VM/JIT/x86-64/JitEmitter-internal.h:613-676`
- Modify: `lib/VM/JIT/x86-64/JitEmitter-internal.cpp:8-12` (include),
  `:150-271` (encoder), `:495-613` (`emitSafeStoreOrSlow`)
- Modify: `lib/VM/JIT/x86-64/JitEmitter.h:1626-1668`, `:1696-1717`
- Modify: `lib/VM/JIT/x86-64/JitEmitter-property.cpp:54-92` and `:202`
  (PutByVal tier), `:1615-1622` (GetByVal comment), `:2345-2374` and
  `:2410` (PutById tier)
- Modify: `test/jit/x86-64/putbyval-inline-emitted.js:33-35, 51-71,
  120-125`, `test/jit/x86-64/putbyid-inline-emitted.js:28-36, 67-167`
- Create: `test/jit/store-boxed-double-byval.js`,
  `test/jit/store-boxed-double-byid.js`,
  `test/jit/store-boxed-double-card.js`,
  `test/jit/store-boxed-double-ygfull.js`,
  `test/jit/store-boxed-double-values.js`

**Interfaces:**
- Produces: `JitCounter::NumPutByValBoxes`, `NumPutByIdBoxes`,
  `NumStoreBoxFallbacks`; the x86-64 `emitSafeStoreOrSlow` and
  `emit_shv_encode_or_slow` signatures of Global Constraints; the five new
  test files with `// REQUIRES: jit, jit-arch-x86-64` (Task 2 lifts it);
  `$S/hv32impl/hv64-ident.sh`, `hv64-cmp.py`, `hv64-sens.py`,
  `gc-check.py`, `codesize.diff` and the base binaries of Step 1, which
  Tasks 2 and 4 use.
- Consumes: `Emitter::allocInYoung(CellKind, uint32_t, const x86::Gp &out,
  const x86::Gp &temp1, const x86::Gp &temp2, const asmjit::Label &)`
  (x86-64/JitEmitter.h:2213-2219; out = the cell; `initGCCell` clobbers
  `temp1`, x86-64/JitEmitter-alloc.cpp:138-149); `emitIncrementCounter
  (JitCounter)` (x86-64/JitEmitter.cpp:587-602: `xScratch` and EFLAGS);
  `emit_sh_cp_encode_non_null` (x86-64/JitEmitter-internal.h:410-416: `sub
  reg, r15` under compressed pointers, nothing otherwise; writes EFLAGS);
  `emit_store_shv` (x86-64/JitEmitter-internal.h:483: 32-bit store when a
  slot is four bytes); `cellSize<C>()` (include/hermes/VM/GCCell.h:34-38);
  `RuntimeOffsets::boxedDoubleValue` (lib/VM/JIT/RuntimeOffsets.h:73),
  `kShvTagBits` (under `HERMESVM_BOXED_DOUBLES`, RuntimeOffsets.h:114-168);
  `HermesValue32::Tag::BoxedDouble == 4`
  (include/hermes/VM/SmallHermesValue.h:256-267); `Emitter::emitCounters_`
  (x86-64/JitEmitter.h:380).

Register and label contract of the new `emitSafeStoreOrSlow` (boxed
doubles), checked against the tiers:
- Entry: live `loc` (slot address) and `value` (HermesValue), to the end.
  `t1`, `t2`, `t3` free and distinct from both. EFLAGS, `xScratch` and the
  vector temps dead: the PutByVal key and its scratch are last used at the
  index guard (x86-64/JitEmitter-property.cpp:135-142); the PutById tier
  has no vector register. `target` is dead but is never used as a
  temporary: it may be a callee-saved global holding its frame register.
- `joinLab` (= the young-target exit and the fall-through of the segment
  test): live `loc`, `value`, `t1` = segment start. The encode writes `t2`
  (the SmallHermesValue) and `t3`; it never writes `t1`.
- `boxLab` (from the encode): live `loc`, `value`; `t1` = segment start,
  not needed (recomputed); `t2`, `t3` hold encode garbage. `allocInYoung`
  writes `t2` (cell), `t3` (new level), `t1` (end, then the debug id and
  `KindAndSize`). Under ASan its call saves the eight GP temps
  (x86-64/JitEmitter-alloc.cpp:41-82), so `loc` and `value` survive in a
  temp or in a callee-saved global; nothing is in `xScratch` or a vector
  register. No GC safepoint between the allocation and the store: the box
  pointer in `t2` is never held across one.
- `slowFullLab`: reached by `ja` from the bump before the level is stored
  (x86-64/JitEmitter-alloc.cpp:38-39, 85); writes `xScratch`/EFLAGS, then
  `jmp slowLab`. The helper path reads only frame slots (the callers sync
  every operand first, x86-64/JitEmitter-property.cpp:412-415, :2480-2482).
- `dirtyCardLab`: `t1` = segment start on both incoming paths (the inline
  path never wrote it after the `and`; the box arm recomputes it); `t2` is
  scratch.
- `doneLab`: falls through into the tier's `jmp contLab`
  (x86-64/JitEmitter-property.cpp:476-477, :2490-2491), so `slowFull` sits
  out of line after the inline path's unconditional `jmp dirtyCard`.

- [ ] **Step 1: BASE and baselines**

Record `git rev-parse HEAD` as BASE. Confirm the JIT sources are those
the root-cause measurement used: `git diff --stat 9f8383072 HEAD -- lib
include` must print nothing (Task 4 reuses that measurement as "before").

Build the reference trees at BASE and keep their binaries (they are linked
statically against Hermes, `readelf -d` shows only system libraries):

```bash
S=/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad
mkdir -p $S/hv32impl/base
cmake --build cmake-build-x86jit --target hermes
cp cmake-build-x86jit/bin/hermes $S/hv32impl/base/hermes-x86jit
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
cp cmake-build-arm64/bin/hermes $S/hv32impl/base/hermes-arm64
for t in cmake-build-jit2 cmake-build-jit2-hv32; do
  cmake --build $t --target hermes hermesc
  mkdir -p $S/hv32impl/base/$t
  cp $t/bin/hermes $t/bin/hermesc $S/hv32impl/base/$t/
done
```

Build code-size measurement copies of the two Release binaries at BASE.
`JIT total memory usage` (lib/VM/JIT/JitCompiler.cpp:632-633, :714) is
allocator usage before installation plus the current body's unrounded
size, and the allocator rounds every installed body to its granularity
(64 bytes by default, external/asmjit/asmjit/src/asmjit/core/
jitallocator.cpp:787, :809-810), so the difference of two consecutive
totals includes the previous body's padding. No existing dump line prints
a body's own size, so a temporary line does. Write
`$S/hv32impl/codesize.diff` exactly (it applies to BASE; never commit it):

```diff
diff --git a/lib/VM/JIT/JitCompiler.cpp b/lib/VM/JIT/JitCompiler.cpp
--- a/lib/VM/JIT/JitCompiler.cpp
+++ b/lib/VM/JIT/JitCompiler.cpp
@@ -712,6 +712,9 @@
 
   if (jc_.dumpJITCode_ & (DumpJitCode::Code | DumpJitCode::CompileStatus)) {
     llvh::outs() << "\nJIT total memory usage (bytes): " << usedSize << "\n";
+    // TEMPORARY, never committed (plan Task 4): this body's own emitted
+    // bytes, without the allocator rounding usedSize includes.
+    llvh::outs() << "JIT code size (bytes): " << em_.code.codeSize() << "\n";
     llvh::outs() << "JIT successfully compiled FunctionID "
                  << codeBlock_->getFunctionID() << ", '" << funcName_ << "'";
     if (version >= 2)
```

```bash
git apply $S/hv32impl/codesize.diff
for t in cmake-build-jit2 cmake-build-jit2-hv32; do
  cmake --build $t --target hermes
  cp $t/bin/hermes $S/hv32impl/base/$t/hermes-codesize
done
git checkout -- lib/VM/JIT/JitCompiler.cpp
git status --short
```

`git status --short` must list only the two untracked files of Global
Constraints. (The jit2 trees now hold instrumented binaries; the next
build of them recompiles JitCompiler.cpp, Task 4 Step 2.)

The HV64 identity tools (spec criterion 2). A dump is compared RAW, from
runs with ASLR off (`setarch $(uname -m) -R`), so heap, stack and JIT
addresses agree between two runs. Two binaries differ legitimately only
where an immediate or a constant-pool word is the address of a helper
inside the binary, which moves when the binary changes. The comparer
therefore resolves every hexadecimal operand and pool word on both sides,
each through its own binary at its own load address, EVEN WHEN THE TWO
NUMBERS ARE EQUAL: a value that lies in a sized symbol of either binary
must resolve on both sides to the same symbol and offset, so a stale
address that kept its number but now names something else (the emitted
call target is the function pointer itself, independent of the comment
naming it, x86-64/JitEmitter.cpp:642-644, arm64/JitEmitter.cpp:603-605,
and a cold call never executes) is reported. A value that resolves in
neither binary must be byte-equal. Everything else must match byte for
byte too: instructions, constants, pool words, heap addresses, labels, and
the `JIT total memory usage` lines (which carry the code size). Checked on
this tree before writing the plan:

- two ASLR-off runs of one binary are byte-identical for all six inputs
  below and for every test/jit/*.js (x86-64 HV64 Debug ASan; arm64 HV64
  under qemu for the six inputs), except the benchmarks' timed score line;
- two different x86-64 HV64 Release builds (cmake-build-release and
  cmake-build-x86jit-rel, glibc malloc) give equal dumps for all six inputs
  with only nm-resolved helper addresses differing (924 of them in
  Box2D), so heap addresses do not move with the binary there;
- with ASLR off the x86-64 load address is 0x555555554000 and the arm64
  guest's under qemu 0x7ffff66a0000 for this binary; the tool derives it
  per binary from `LD_SHOW_AUXV` (AT_ENTRY minus the ELF entry point);
- the comparer reports one altered constant, one helper address swapped
  for another helper's, a changed total-memory line, and a heap address it
  cannot resolve (unsized symbols such as `_end` are skipped, so a brk-heap
  address just past the image never resolves);
- stale addresses: a Debug x86-64 dump relocated symbol by symbol to a
  Release binary's layout is accepted against that binary, and the same
  relocated dump with one helper left at its Debug address -- numerically
  unchanged, but in the Release binary not that helper -- is reported. The
  previous comparator, which skipped equal numbers, accepted it. Across the
  two Release builds above, all 4583 resolved addresses in Box2D
  (924 moved, the rest numerically equal) resolve to the same symbols.

Not checked, because no second arm64 HV64 build exists: whether the arm64
guest's heap addresses move with the guest image. If Task 2 or Task 4
reports a `DIFFERENT` whose only differences are unresolved heap
addresses on arm64, do not normalize them: stop and report to the
controller with the differing lines.

Write `$S/hv32impl/hv64-cmp.py`:

```python
#!/usr/bin/env python3
"""Strict comparison of two raw -Xdump-jitcode dumps of one input, made by
two hermes binaries with ASLR off (spec criterion 2).

The lines must have the same shape: equal once every hexadecimal token is
blanked out. Each pair of hexadecimal tokens is then judged on what it
names, resolved through each side's OWN binary, even when the two numbers
are equal: if either value lies inside a sized symbol of its binary, both
must resolve, through nm at their own load address, to the same symbol and
offset (a helper that moved is accepted; a stale address that now names
something else is not, numerically equal or not). A pair that resolves in
neither binary -- a constant, a heap address -- must be byte-equal.
Everything else fails: an instruction, a constant, a pool word, a size, a
heap address.

Usage: hv64-cmp.py BEFORE.dump AFTER.dump BEFORE_BIN BEFORE_BASE
                   AFTER_BIN AFTER_BASE NM
BASE is the binary's load address in hex. Exit 0 identical, 1 different.
"""
import bisect
import re
import subprocess
import sys

HEX = re.compile(r"0x[0-9A-Fa-f]+")
# nm symbol types that name a location in the image.
TYPES = set("TtWwDdBbRrVvi")


def symtab(binary, base, nm):
    """The sized symbols of BINARY, as sorted (start, end, name) lists
    at load address BASE. Unsized symbols (_end, __bss_start, ...) are
    skipped, so that a heap address just past the image never resolves."""
    out = subprocess.run(
        [nm, "-n", "-S", "--defined-only", binary],
        check=True, capture_output=True, text=True).stdout
    starts, ends, names = [], [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) != 4 or p[2] not in TYPES or p[3].startswith("$"):
            continue
        start = int(p[0], 16) + base
        starts.append(start)
        ends.append(start + int(p[1], 16))
        names.append(p[3])
    return starts, ends, names


def resolve(tab, value):
    """(name, offset) of the sized symbol containing VALUE, or None."""
    starts, ends, names = tab
    i = bisect.bisect_right(starts, value) - 1
    if i < 0 or value >= ends[i]:
        return None
    return names[i], value - starts[i]


def main():
    bd, ad, bb, bbase, ab, abase, nm = sys.argv[1:8]
    bt = symtab(bb, int(bbase, 16), nm)
    at = symtab(ab, int(abase, 16), nm)
    bl = open(bd, errors="replace").read().splitlines()
    al = open(ad, errors="replace").read().splitlines()
    diffs, resolved = [], []
    if len(bl) != len(al):
        diffs.append("line count %d != %d" % (len(bl), len(al)))
    for n, (x, y) in enumerate(zip(bl, al), 1):
        if HEX.sub("#", x) != HEX.sub("#", y):
            diffs.append("%d: %r != %r" % (n, x, y))
            continue
        for tx, ty in zip(HEX.findall(x), HEX.findall(y)):
            rx, ry = resolve(bt, int(tx, 16)), resolve(at, int(ty, 16))
            if rx is None and ry is None:
                if tx != ty:
                    diffs.append("%d: %s != %s in %r" % (n, tx, ty, x))
            elif rx == ry:
                resolved.append((n, rx, tx != ty))
            else:
                diffs.append("%d: %s (%s) != %s (%s) in %r" % (
                    n, tx, rx, ty, ry, x))
    for d in diffs[:40]:
        print("  " + d)
    names = sorted(set("%s+%d" % r for _, r, _ in resolved))
    print("  %d lines, %d helper addresses resolved (%d moved; %s), "
          "%d differences" % (
              len(al), len(resolved), sum(m for _, _, m in resolved),
              ", ".join(names[:4]) + (" ..." if len(names) > 4 else ""),
              len(diffs)))
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
```

`$S/hv32impl/hv64-sens.py`:

```python
#!/usr/bin/env python3
"""Sensitivity of hv64-cmp.py: deliberately altered copies of a dump, made
by BIN (loaded at BASE), that it must report -- and one it must accept.

Same binary on both sides (BIN against BIN):
  const   the first segment mask 0xFFFFFFFFFFC00000 becomes
          0xFFFFFFFFFF800000;                               must be reported
  helper  the first immediate that resolves to the start of a function
          becomes another function's address;              must be reported
With a second, differently laid out binary OTHER (loaded at OBASE), the
dump as OTHER would emit it, BIN against OTHER:
  moved   every resolved address rewritten to the same symbol and offset
          in OTHER;                                         must be accepted
  stale   the same, except one helper keeps BIN's numeric address, which
          in OTHER names something else;                   must be reported

Usage: hv64-sens.py DUMP BIN BASE NM OUTDIR [OTHER OBASE]
Exit 0 if every alteration is judged as stated, 2 if a case cannot be
constructed (no segment mask, too few helpers, or no helper whose address
differs between BIN and OTHER), 1 otherwise.
"""
import importlib.util
import os
import re
import subprocess
import sys

HEX = re.compile(r"0x[0-9A-Fa-f]+")

dump, binary, base, nm, out = sys.argv[1:6]
other, obase = (sys.argv[6], sys.argv[7]) if len(sys.argv) > 7 else (None, None)
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "hv64cmp", os.path.join(here, "hv64-cmp.py"))
hv64cmp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hv64cmp)

text = open(dump, errors="replace").read()
tab = hv64cmp.symtab(binary, int(base, 16), nm)
cases = []  # (name, altered text, after binary, after base, expected rc)

if "0xFFFFFFFFFFC00000" not in text:
    print("no segment mask in " + dump, file=sys.stderr)
    sys.exit(2)
cases.append(("const",
              text.replace("0xFFFFFFFFFFC00000", "0xFFFFFFFFFF800000", 1),
              binary, base, 1))
helpers = {}
for tok in HEX.findall(text):
    r = hv64cmp.resolve(tab, int(tok, 16))
    if r is not None and r[1] == 0:
        helpers.setdefault(r[0], tok)
if len(helpers) < 2:
    print("fewer than two helper addresses in " + dump)
    sys.exit(2)
(n1, t1), (n2, t2) = list(helpers.items())[:2]
cases.append(("helper", text.replace(t1, t2, 1), binary, base, 1))

if other:
    otab = hv64cmp.symtab(other, int(obase, 16), nm)
    where = {}
    for s, n in zip(otab[0], otab[2]):
        where.setdefault(n, s)
    stale = None
    missing = set()

    def move(m):
        global stale
        tok = m.group(0)
        r = hv64cmp.resolve(tab, int(tok, 16))
        if r is None:
            return tok
        if r[0] not in where:
            missing.add(r[0])
            return tok
        if stale is None and r[1] == 0 and \
                hv64cmp.resolve(otab, int(tok, 16)) != r:
            stale = tok
        return "0x%0*X" % (len(tok) - 2, where[r[0]] + r[1])

    moved = HEX.sub(move, text)
    if missing:
        print("symbols missing in OTHER: " + ", ".join(sorted(missing)[:4]))
        sys.exit(2)
    if stale is None:
        print("no helper whose address differs between BIN and OTHER")
        sys.exit(2)
    r = hv64cmp.resolve(tab, int(stale, 16))
    new = "0x%0*X" % (len(stale) - 2, where[r[0]] + r[1])
    cases.append(("moved", moved, other, obase, 0))
    cases.append(("stale", moved.replace(new, stale, 1), other, obase, 1))

ok = True
for name, alt, abin, abase, want in cases:
    path = os.path.join(out, "sens-%s.txt" % name)
    with open(path, "w") as f:
        f.write(alt)
    rc = subprocess.run(
        [sys.executable, "-I", os.path.join(here, "hv64-cmp.py"), dump, path,
         binary, base, abin, abase, nm],
        capture_output=True).returncode
    verdict = {0: "ACCEPTED", 1: "REPORTED"}.get(rc, "ERROR")
    good = rc == want
    print("alteration %s: %s%s" % (name, verdict, "" if good else "  <-- WRONG"))
    ok = ok and good
sys.exit(0 if ok else 1)
```

and `$S/hv32impl/hv64-ident.sh`:

```bash
#!/bin/bash
# HV64 emitted-code identity (spec criterion 2). Dumps every input with its
# own flags under two hermes binaries with ASLR off, raw, and compares them
# with hv64-cmp.py: byte for byte, except helper addresses that nm resolves
# to the same symbol in both binaries.
# Usage: hv64-ident.sh BEFORE AFTER OUTDIR [--qemu] [--corpus]
#   --qemu    run aarch64 binaries under qemu-aarch64-static
#   --corpus  also every test/jit/*.js, with jit-dump.sh's flags
set -u
BEFORE=$1; AFTER=$2; OUT=$3; shift 3
QEMU=(); NM=nm; CORPUS=0
for a in "$@"; do
  case "$a" in
    --qemu) QEMU=(qemu-aarch64-static -L /usr/aarch64-linux-gnu)
            NM=aarch64-linux-gnu-nm ;;
    --corpus) CORPUS=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
R=/home/tmikov/work/hermes-x86-jit
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NOASLR=(setarch "$(uname -m)" -R)
mkdir -p "$OUT"
echo 'print(1)' > "$OUT/one.js"

# The load address of a binary with ASLR off: AT_ENTRY minus the ELF entry.
imgbase() {
  local entry at
  entry=$(readelf -h "$1" | awk '/Entry point address:/ {print $4}')
  at=$("${NOASLR[@]}" env LD_SHOW_AUXV=1 ${QEMU[@]+"${QEMU[@]}"} "$1" \
       "$OUT/one.js" 2>&1 | awk '/^AT_ENTRY:/ {print $2}')
  python3 -c "print(hex($at - $entry))"
}
BB=$(imgbase "$BEFORE"); AB=$(imgbase "$AFTER")
echo "image base: before $BB, after $AB"

status=0
one() {
  local name=$1 file=$2; shift 2
  local side bin rc
  for side in before after; do
    bin=$BEFORE
    [ "$side" = after ] && bin=$AFTER
    "${NOASLR[@]}" ${QEMU[@]+"${QEMU[@]}"} "$bin" "$@" -Xdump-jitcode=3 \
      "$file" > "$OUT/$name.$side.raw" 2>&1
    rc=$?
    # The benchmarks print a timed score; it is program output, not JIT
    # output, and the only line the control runs found to vary.
    grep -v -E '^(NavierStokes|Box2D) [0-9]+$' "$OUT/$name.$side.raw" \
      > "$OUT/$name.$side.txt"
    if [ $rc -ne 0 ]; then
      echo "RUN FAILED ($rc): $name ($side)"
      status=1
    fi
  done
  if python3 -I "$HERE/hv64-cmp.py" "$OUT/$name.before.txt" \
       "$OUT/$name.after.txt" "$BEFORE" "$BB" "$AFTER" "$AB" "$NM" \
       > "$OUT/$name.cmp" 2>&1; then
    echo "IDENTICAL $name: $(tail -1 "$OUT/$name.cmp")"
  else
    echo "DIFFERENT $name:"
    cat "$OUT/$name.cmp"
    status=1
  fi
}
one putbyval-x86 "$R/test/jit/x86-64/putbyval-inline-emitted.js" \
  -fno-inline -Xjit=force
one putbyid-x86 "$R/test/jit/x86-64/putbyid-inline-emitted.js" \
  -fno-inline -Xjit -Xjit-threshold=4
one putbyval-arm64 "$R/test/jit/putbyval-inline-emitted-arm64.js" \
  -fno-inline -Xjit=force -Xjit-recompile-threshold=64
one putbyid-arm64 "$R/test/jit/putbyid-inline-emitted-arm64.js" \
  -fno-inline -Xjit -Xjit-threshold=4
one navier-stokes "$R/benchmarks/octane/navier-stokes.js" \
  -Xjit -Xjit-max-recompiles=0
one box2d "$R/benchmarks/octane/box2d.js" -Xjit -Xjit-max-recompiles=0
if [ $CORPUS -eq 1 ]; then
  # Every JIT test, as utils/jit/jit-dump.sh runs them: forced, first
  # versions only, -typed and -Xhermes-internal-test-methods where the
  # file's own RUN lines use them.
  for f in "$R"/test/jit/*.js; do
    flags=(-Xjit=force -Xjit-threshold=1 -Xjit-max-recompiles=0)
    grep -qE '^// *RUN:.*[[:space:]]-typed([[:space:]]|$)' "$f" &&
      flags+=(-typed)
    grep -qE '^// *RUN:.*-Xhermes-internal-test-methods' "$f" &&
      flags+=(-Xhermes-internal-test-methods)
    one "corpus-$(basename "$f" .js)" "$f" "${flags[@]}"
  done
fi
exit $status
```

The inputs are the four emitted-code tests' programs with their own RUN
flags, the two benchmarks with recompilation off (a recompile's timing can
follow the score loop; first versions do not), and with `--corpus` every
JIT test the way `utils/jit/jit-dump.sh` runs them. The score line is the
only line dropped, and only from the two benchmarks' output.

Controls on the base binaries (stability, then sensitivity):

```bash
chmod +x $S/hv32impl/hv64-ident.sh
$S/hv32impl/hv64-ident.sh $S/hv32impl/base/hermes-x86jit \
  $S/hv32impl/base/hermes-x86jit $S/hv32impl/ident-control-x86 --corpus |
  tee $S/hv32impl/ident-control-x86.log
$S/hv32impl/hv64-ident.sh $S/hv32impl/base/hermes-arm64 \
  $S/hv32impl/base/hermes-arm64 $S/hv32impl/ident-control-arm64 --qemu |
  tee $S/hv32impl/ident-control-arm64.log
XB=$(awk '/^image base:/ {print $4}' $S/hv32impl/ident-control-x86.log | tr -d ,)
AB=$(awk '/^image base:/ {print $4}' $S/hv32impl/ident-control-arm64.log | tr -d ,)
# The load address, with ASLR off, of a second x86-64 binary laid out
# differently: the base Release HV64 build.
O=$S/hv32impl/base/cmake-build-jit2/hermes
OE=$(setarch $(uname -m) -R env LD_SHOW_AUXV=1 $O \
  $S/hv32impl/ident-control-x86/one.js 2>&1 | awk '/^AT_ENTRY:/ {print $2}')
OB=$(python3 -c "print(hex($OE - $(readelf -h $O | awk '/Entry point address:/ {print $4}')))")
python3 -I $S/hv32impl/hv64-sens.py \
  $S/hv32impl/ident-control-x86/putbyid-x86.after.txt \
  $S/hv32impl/base/hermes-x86jit $XB nm $S/hv32impl/ident-control-x86 \
  $O $OB
python3 -I $S/hv32impl/hv64-sens.py \
  $S/hv32impl/ident-control-arm64/putbyval-arm64.after.txt \
  $S/hv32impl/base/hermes-arm64 $AB aarch64-linux-gnu-nm \
  $S/hv32impl/ident-control-arm64
```

Expected: every line `IDENTICAL ... (0 moved; ...), 0 differences`
(x86-64: 6 inputs plus every JIT test, about a minute; arm64: 6 inputs,
about a minute under qemu). The x86-64 sensitivity run prints `const:
REPORTED`, `helper: REPORTED`, `moved: ACCEPTED`, `stale: REPORTED` and
exits 0; the arm64 one, which has no second arm64 binary yet, prints the
first two (its stale case runs in Task 2 Step 8 against the head binary).
If a control differs,
find the varying line; if it is program output rather than JIT output, add
an input-specific `grep -v` with the exact pattern next to the score-line
one and record it in the report; anything else is a finding to report.

The GC checker of Step 17, `$S/hv32impl/gc-check.py`:

```python
#!/usr/bin/env python3
"""Check the GC conditions of a counting test (spec "Testing") from the
-gc-print-collection-stats output of one run, read on stdin.

Every collection is a record, in the order the collections completed. A
young collection that started an old-generation collection carries the tag
"old gen start" whether or not that collection later completed, so a start
at the end of the last gc() is visible even when the process exits first.

The measured region of a test lies between its last gc() and its first
non-forced ("natural") collection, so the forced prefix must hold exactly
NGC gc() calls -- per call one young collection tagged "old gen start", one
completed "old" collection, and one more young collection that starts
nothing -- and nothing natural may precede it. Its peak old-generation
allocated plus external bytes must stay under 7 MiB (spec condition 1).

Usage: gc-check.py NGC [--natural-after]
  --natural-after  natural collections are expected after the measured
                   region (the card test's churn); at least one young one.
Exit 0 if every condition holds.
"""
import json
import sys

LIMIT = 7 << 20


def main():
    ngc = int(sys.argv[1])
    natural_after = "--natural-after" in sys.argv[2:]
    text = sys.stdin.read()
    at = text.index("Collections:")
    recs, _ = json.JSONDecoder().raw_decode(text[text.index("[", at):])
    pre = []
    for r in recs:
        if r["cause"] != "forced":
            break
        pre.append(r)
    post = recs[len(pre):]
    young = [r for r in pre if r["collectionType"] == "young"]
    old = [r for r in pre if r["collectionType"] == "old"]
    starts = [r for r in pre if "old gen start" in r["tags"]]
    peak = max(
        max(r["preAllocated"] + r["preExternal"],
            r["postAllocated"] + r["postExternal"]) for r in pre) if pre else 0
    print("forced: %d young, %d old, %d old-gen starts, %d compactions; "
          "peak allocated+external %d B; after: %d (%s)" % (
              len(young), len(old), len(starts),
              sum("compact" in r["tags"] for r in pre), peak, len(post),
              ", ".join("%s/%s/%s" % (r["collectionType"], r["cause"],
                                      "+".join(r["tags"])) for r in post[:4])))
    errs = []
    if (len(young), len(old), len(starts)) != (2 * ngc, ngc, ngc):
        errs.append("expected %d young, %d old, %d starts in the forced "
                    "prefix" % (2 * ngc, ngc, ngc))
    if [r for r in young[1::2] if "old gen start" in r["tags"]]:
        errs.append("a gc()'s second young collection started an old-gen "
                    "collection")
    if peak >= LIMIT:
        errs.append("peak allocated+external %d B >= 7 MiB" % peak)
    if any(r["cause"] == "forced" for r in post):
        errs.append("a forced collection after a natural one")
    if natural_after:
        if not any(r["collectionType"] == "young" for r in post):
            errs.append("no young collection after the measured region")
    elif post:
        errs.append("natural collections; none expected")
    for e in errs:
        print("FAIL: " + e)
    sys.exit(1 if errs else 0)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Invoke the gc-safe-coding skill.**

- [ ] **Step 3: The counters**

In `include/hermes/VM/JIT/JitCounters.h`, replace the last entry (lines
44-46):

```cpp
  /* Inline hits of the GetById string blocks (Length and Method), \
   * counted by the emitted code only with -Xjit-emit-counters. */ \
  X(NumByIdStringHits)
```

with

```cpp
  /* Inline hits of the GetById string blocks (Length and Method), \
   * counted by the emitted code only with -Xjit-emit-counters. */ \
  X(NumByIdStringHits)                                             \
  /* Doubles the PutByVal JSArray tier boxed in a young-generation \
   * BoxedDouble and stored inline (HERMESVM_BOXED_DOUBLES only),  \
   * counted by the emitted code only with -Xjit-emit-counters. */ \
  X(NumPutByValBoxes)                                              \
  /* The same for the PutById inline tier. */                      \
  X(NumPutByIdBoxes)                                               \
  /* Box allocations of either store tier that found the young     \
   * generation full and declined to the helper, counted by the    \
   * emitted code only with -Xjit-emit-counters. */                \
  X(NumStoreBoxFallbacks)
```

(Re-align the backslashes if `git clang-format` asks.) Appending changes
no existing check: the counter tests match counters by name and none has
`CHECK-NEXT` after `NumByIdStringHits` (grep `test/jit` for
`NumByIdStringHits` before committing to confirm).

- [ ] **Step 4: The PutByVal counting test**

Create `test/jit/store-boxed-double-byval.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- young 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- old 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter boxes inline (plan Task 2).
// UNSUPPORTED: handle_san || gc_malloc

// The PutByVal JSArray tier stores a double that needs a BoxedDouble without
// leaving JIT code: it allocates the box in the young generation and stores
// the tagged pointer (doc/superpowers/specs/2026-10-29-jit-inline-boxed-
// double-store-design.md). Under HV32 and BOXED each of the N measured
// stores boxes, so NumPutByValBoxes is N; under HV64 nothing boxes and it
// is 0. Run twice: the measured array young ("young"), and promoted to the
// old generation by a second gc() ("old"), where every box store also
// dirties a card.
//
// The values are 1 + (2k + 1) * 2^-52: exact doubles whose lowest mantissa
// bit is set, so their low 35 bits (HV32) and low 3 bits (BOXED) are
// nonzero, and distinct for distinct k.
//
// Why exactly N. -Xjit=force compiles every function at its first call.
// (a) The warm-up call stores the inline-encodable 1 into a throwaway
// array: no box. (b) gc() leaves the young generation empty and, for the one
// or two calls made here, no old-generation collection running and no
// compactee (spec "Testing"; checked on every tree from
// -gc-print-collection-stats, plan Task 1 Step 17). Were either active, an
// old-target store would decline before the box arm and the count would
// fall short: a false failure, never a masked pass.
// (c) The measured array holds N small integers, so every index passes the
// tier's range guard and no element is a hole. (d) The measured loop
// allocates N boxes (24 KB at the Debug box size) and nothing else, far
// below the smallest young generation, so no collection starts inside it
// and every store passes the barrier predicate: the box arm runs N times.
// The DUMP lines show nothing is compiled from "measure" on. No other JIT
// PutByVal stores a boxing double (the array is filled by push; the
// read-back is GetByVal), and only the box arm counts.
//
// Fails before the feature (0 under HV32 and BOXED: every store declined),
// and with the box arm sent to the helper (plan Task 1, mutation A).

function storeElem(arr, i, v) {
  arr[i] = v;
}

function main(store, gc, old) {
  var N = 1000;
  var eps = Number.EPSILON;
  store([0], 0, 1);
  gc();
  var arr = [];
  for (var i = 0; i < N; ++i)
    arr.push(i);
  if (old)
    gc();
  print('measure');
  for (var i = 0; i < N; ++i)
    store(arr, i, 1 + (2 * i + 1) * eps);
  var bad = 0;
  for (var i = 0; i < N; ++i)
    if (arr[i] !== 1 + (2 * i + 1) * eps)
      ++bad;
  print('mismatches', bad);
}

main(storeElem, gc, hermescli.getScriptArgs()[0] === 'old');

// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeElem'
// CHECK: {{^}}measure{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}mismatches 0{{$}}
// CHECK: JIT counters:
// CHECK-HV64: NumPutByValBoxes: 0{{$}}
// CHECK-HV32: NumPutByValBoxes: 1000{{$}}
// CHECK-BOXED: NumPutByValBoxes: 1000{{$}}
// CHECK-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}
```

- [ ] **Step 5: The PutById counting test**

Create `test/jit/store-boxed-double-byid.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- young 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- old 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter boxes inline (plan Task 2).
// UNSUPPORTED: handle_san || gc_malloc

// The PutById tier boxes a double inline, as the PutByVal tier does in
// store-boxed-double-byval.js: NumPutByIdBoxes is N under HV32 and BOXED, 0
// under HV64, with the measured object young and then old.
//
// Threshold mode, because the PutById tier is emitted only when the site's
// write cache names a class at compile time. setP has no loop, so with
// -Xjit-threshold=4 its first four calls run interpreted (filling the cache
// with make()'s class) and the fifth compiles it, tier included
// (x86-64/JIT.h:289-292, Interpreter.cpp:1280). The warm-up makes five calls
// with inline-encodable integers: no box. main has a loop and runs once, so
// it stays interpreted: its own stores and reads count nothing. The DUMP
// lines show setP compiled before "measure" and nothing compiled after it,
// so every measured store runs setP's compiled body.
//
// Why exactly N: as in store-boxed-double-byval.js; the measured object comes
// from the same make() as the warm-up one, so it has the cached class, and
// p is a direct slot. Fails before the feature, and with the box arm sent
// to the helper (plan Task 1, mutation A).

function setP(o, v) {
  o.p = v;
}

function main(setP, gc, old) {
  var N = 1000;
  var eps = Number.EPSILON;
  function make() {
    return {p: 0};
  }
  var w = make();
  for (var i = 0; i < 5; ++i)
    setP(w, i);
  gc();
  var o = make();
  if (old)
    gc();
  print('measure');
  var bad = 0;
  for (var i = 0; i < N; ++i) {
    var v = 1 + (2 * i + 1) * eps;
    setP(o, v);
    if (o.p !== v)
      ++bad;
  }
  print('mismatches', bad);
}

main(setP, gc, hermescli.getScriptArgs()[0] === 'old');

// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'setP'
// CHECK: {{^}}measure{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}mismatches 0{{$}}
// CHECK: JIT counters:
// CHECK: NumPutByValBoxes: 0{{$}}
// CHECK-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-HV32-NEXT: NumPutByIdBoxes: 1000{{$}}
// CHECK-BOXED-NEXT: NumPutByIdBoxes: 1000{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}
```

- [ ] **Step 6: The card test**

Create `test/jit/store-boxed-double-card.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- byval 2>&1 | %FileCheck --check-prefixes=CHECK,BYVAL-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- byid 2>&1 | %FileCheck --check-prefixes=CHECK,BYID-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- byidwide 2>&1 | %FileCheck --check-prefixes=CHECK,BYID-%hv-mode %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter boxes inline (plan Task 2).
// UNSUPPORTED: handle_san || gc_malloc

// An old target that receives one inline box gets its card dirtied. The box
// is young by construction, so the store is an old-to-young pointer; if the
// box arm skipped the card, HERMES_SLOW_DEBUG's HadesGC::verifyCardTable()
// (run at the start of every young collection) finds a young pointer in an
// old slot on a clean card, or the read-back after the collection sees a
// freed box. One case per process, so that one case cannot dirty the other's
// card: byval (an element of an old array), byid (a direct slot of an old
// object), byidwide (an out-of-line slot, inside the old object's
// propStorage cell; `p` is the eighth property).
//
// Not masked: the second gc()'s young collection clears every old segment's
// cards (HadesGC.cpp:3100-3110) with no old-generation collection running
// (checked with -gc-print-collection-stats), the case stores exactly one
// value into an old object after it, and every function is compiled
// before "warm" (the
// DUMP lines check that nothing compiles after it, so no compilation writes
// an old object either). The only store that can dirty the target's card is
// the box arm's. churn() then allocates more than the largest young
// generation holds (300,000 objects of at least 16 bytes is over 4 MiB),
// forcing a young collection. Fails with the box arm's card dirty removed
// (plan Task 1, mutation B), under HERMES_SLOW_DEBUG.

function storeElem(arr, i, v) {
  arr[i] = v;
}

function setP(o, v) {
  o.p = v;
}

function main(storeElem, setP, gc, which) {
  var v = 1 + 3 * Number.EPSILON;
  function makeArr() {
    return [0, 0, 0, 0];
  }
  function makeObj() {
    return {p: 0};
  }
  function makeWide() {
    return {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, p: 0};
  }
  function alloc(i) {
    return {a: i};
  }
  function churn(n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += alloc(i).a;
    return s;
  }
  var make =
      which === 'byval' ? makeArr : which === 'byid' ? makeObj : makeWide;
  // Compile the store function (five calls), alloc (five calls) and churn
  // (two calls) before anything is measured.
  var w = make();
  for (var i = 0; i < 5; ++i) {
    if (which === 'byval')
      storeElem(w, 0, i);
    else
      setP(w, i);
  }
  churn(5);
  churn(5);
  print('warm');
  gc();
  var t = make();
  gc();
  if (which === 'byval')
    storeElem(t, 0, v);
  else
    setP(t, v);
  var s = churn(300000);
  print('value', (which === 'byval' ? t[0] : t.p) === v, s);
}

main(storeElem, setP, gc, hermescli.getScriptArgs()[0]);

// CHECK: {{^}}warm{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}value true 44999850000{{$}}
// CHECK: JIT counters:
// BYVAL-HV64: NumPutByValBoxes: 0{{$}}
// BYVAL-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// BYVAL-HV32: NumPutByValBoxes: 1{{$}}
// BYVAL-HV32-NEXT: NumPutByIdBoxes: 0{{$}}
// BYVAL-BOXED: NumPutByValBoxes: 1{{$}}
// BYVAL-BOXED-NEXT: NumPutByIdBoxes: 0{{$}}
// BYID-HV64: NumPutByValBoxes: 0{{$}}
// BYID-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// BYID-HV32: NumPutByValBoxes: 0{{$}}
// BYID-HV32-NEXT: NumPutByIdBoxes: 1{{$}}
// BYID-BOXED: NumPutByValBoxes: 0{{$}}
// BYID-BOXED-NEXT: NumPutByIdBoxes: 1{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}
```

(`44999850000` is the sum of 0..299999. The 16-byte lower bound is the
Release `BoxedDouble`-sized cell; every `JSObject` is larger, and Step 17
checks with `-gc-print-collection-stats` that a young collection did
happen after the measured store.)

- [ ] **Step 7: The young-generation-full test**

Create `test/jit/store-boxed-double-ygfull.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s | %FileCheck --check-prefix=CHECK %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter boxes inline (plan Task 2).
// UNSUPPORTED: handle_san || gc_malloc

// A box allocation that finds the young generation full declines to the
// helper with nothing stored, and the helper completes the store. The loop
// overwrites slot 0 of a small array with a fresh boxing value and reads it
// back each time; it allocates nothing but boxes, and runs 2 * 4 MiB / 16 =
// 524,288 times: two fills of the largest young generation at the Release
// box size. The first fallback is deterministic: the array is young until
// the first collection, so every store until then takes the young arm, and
// the box allocation is the only allocation that can find the young
// generation full. With counters (first RUN) the fallback goes through the
// counting block; without (second RUN) slowFull is the helper label itself.
// After the first collection the array is old, so the later box stores also
// dirty its card, which the next young collection verifies under
// HERMES_SLOW_DEBUG.
//
// Fails if the fallback skipped the helper (mutation E: a mismatch), and if
// it were not counted (mutation F: NumStoreBoxFallbacks 0).

function storeElem(arr, i, v) {
  arr[i] = v;
}

function main(store) {
  var N = 524288;
  var eps = Number.EPSILON;
  var arr = [0, 0];
  var bad = 0;
  for (var k = 0; k < N; ++k) {
    var v = 1 + (2 * k + 1) * eps;
    store(arr, 0, v);
    if (arr[0] !== v)
      ++bad;
  }
  print('mismatches', bad);
}

main(storeElem);

// CHECK: {{^}}mismatches 0{{$}}
// CHECK-HV64: NumPutByValBoxes: 0{{$}}
// CHECK-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-HV64-NEXT: NumStoreBoxFallbacks: 0{{$}}
// CHECK-HV32: NumPutByValBoxes: {{[1-9][0-9]*$}}
// CHECK-HV32-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-HV32-NEXT: NumStoreBoxFallbacks: {{[1-9][0-9]*$}}
// CHECK-BOXED: NumPutByValBoxes: {{[1-9][0-9]*$}}
// CHECK-BOXED-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-BOXED-NEXT: NumStoreBoxFallbacks: {{[1-9][0-9]*$}}
```

- [ ] **Step 8: The values test (Review Focus 1 and 3)**

Create `test/jit/store-boxed-double-values.js`:

```js
/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline %s > %t.int
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=4 %s > %t.force && diff %t.int %t.force
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-recompile-threshold=4 %s > %t.warm && diff %t.int %t.warm
// RUN: %hermes -fno-inline %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=4 -Xdump-jitcode=3 %s | %FileCheck --check-prefixes=DUMP,DUMP-%hv-mode %s
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter boxes inline (plan Task 2).
// UNSUPPORTED: handle_san || gc_malloc

// Values at the edges of the SmallHermesValue encoding, stored through the
// PutByVal JSArray tier, the PutById tier (a direct and an out-of-line
// slot) and a duo PutByVal site, into young and old targets, and read back
// before and after a collection. They must come back bit-exact (same():
// Object.is semantics). Boxed in both boxed modes: +-0.1, +-5e-324 (bits
// 0x1), 1e-310 (subnormal), +-DBL_MAX, 2^53 - 1, 1 + 2^-52. Boxed under HV32
// only: +-(2^28 - 1), whose low 3 bits are zero. Inline everywhere: 0.5,
// -0, NaN, Infinity.
//
// storeDuo first stores into a Float64Array: its four declines cross
// -Xjit-recompile-threshold=4, and version 2 puts the typed-array tier
// ahead of the JSArray tier (the x86-64 tier's highest register pressure).
// The DUMP lines check that version 2 still has the box arm.
//
// Fails if the box arm stored only part of the double (plan Task 1,
// mutation D: the JIT output differs from the interpreter's); the DUMP
// lines fail before the feature.

function storeElem(arr, i, v) {
  arr[i] = v;
}
function setP(o, v) {
  o.p = v;
}
function setQ(o, v) {
  o.q = v;
}
function storeDuo(t, i, v) {
  t[i] = v;
}

function same(a, b) {
  if (a !== a)
    return b !== b;
  return a === b && (a !== 0 || 1 / a === 1 / b);
}

function show(v) {
  return v === 0 && 1 / v < 0 ? '-0' : String(v);
}

function main(values, gc) {
  var n = values.length;
  function makeObj() {
    return {p: 0};
  }
  function makeWide() {
    return {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, q: 0};
  }
  function makeArr() {
    var a = [];
    for (var i = 0; i < n; ++i)
      a.push(0);
    return a;
  }
  // Warm the PutById write caches (threshold mode compiles at the fifth
  // call) with inline-encodable values.
  var wo = makeObj(), ww = makeWide(), wa = makeArr();
  for (var i = 0; i < 5; ++i) {
    setP(wo, i);
    setQ(ww, i);
    storeElem(wa, 0, i);
  }
  var f64 = new Float64Array(4);
  for (var i = 0; i < 8; ++i)
    storeDuo(f64, 1, i);

  function pass(label, old) {
    var os = [], ws = [], a = makeArr(), d = makeArr();
    for (var i = 0; i < n; ++i) {
      os.push(makeObj());
      ws.push(makeWide());
    }
    if (old)
      gc();
    for (var i = 0; i < n; ++i) {
      var v = values[i];
      setP(os[i], v);
      setQ(ws[i], v);
      storeElem(a, i, v);
      storeDuo(d, i, v);
    }
    function check() {
      var bad = 0;
      for (var i = 0; i < n; ++i) {
        var v = values[i];
        if (!same(os[i].p, v) || !same(ws[i].q, v) || !same(a[i], v) ||
            !same(d[i], v))
          ++bad;
      }
      return bad;
    }
    var before = check();
    gc();
    print(label, 'mismatches', before, check());
    print(label, a.map(show).join(' '));
  }
  pass('young', false);
  pass('old', true);
}

main(
    [0.1, -0.1, 5e-324, -5e-324, 1e-310, 1.7976931348623157e308,
     -1.7976931348623157e308, 268435455, -268435455, 9007199254740991,
     1.0000000000000002, 0.5, -0, NaN, Infinity],
    gc);

// CHECK: young mismatches 0 0
// CHECK-NEXT: young 0.1 -0.1 5e-324 -5e-324 1e-310 1.7976931348623157e+308 -1.7976931348623157e+308 268435455 -268435455 9007199254740991 1.0000000000000002 0.5 -0 NaN Infinity
// CHECK-NEXT: old mismatches 0 0
// CHECK-NEXT: old 0.1 -0.1 5e-324 -5e-324 1e-310 1.7976931348623157e+308 -1.7976931348623157e+308 268435455 -268435455 9007199254740991 1.0000000000000002 0.5 -0 NaN Infinity

// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'storeDuo' (version 2)
// DUMP: // Inline typed array store (kind {{[0-9]+}})
// DUMP: // Inline fast array store
// DUMP-HV32: // Box the double in the young generation
// DUMP-BOXED: // Box the double in the young generation
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeDuo' (version 2)
```

(The interpreter's `String()` of these values is what the CHECK lines
spell; if a spelling differs, the interpreter output is the reference: fix
the CHECK line, never the program.)

- [ ] **Step 9: The x86-64 emitted-code pins**

In `test/jit/x86-64/putbyval-inline-emitted.js`, replace the header
sentence (lines 33-35)

```
// than 32, so the kind has to be masked off the jumbo-cell gate's load. The
// value encoding the two boxed modes add is pinned by
// putbyid-inline-emitted.js, which shares this predicate.
```

with

```
// than 32, so the kind has to be masked off the jumbo-cell gate's load. The
// two boxed modes also encode the value, inside the barrier predicate, and
// box a double that needs it; the encode's dispatch is pinned in full by
// putbyid-inline-emitted.js, which shares this predicate, and its position
// and the box arm are pinned here too.
```

Then replace the block from `// SPEC-LABEL:store:` (line 51) through
`// SPEC-BOXED: jne [[SLOW]]` (line 71) with:

```
// SPEC-LABEL:store:
// SPEC: // Inline fast array store
//
// The shape guards, in the order putByValWithReceiver_RJS makes them. In
// every mode the object check is the tier's first exit, so it is where the
// helper label gets its name: the value is no longer encoded ahead of the
// guards, but inside the barrier predicate below.
// The target must be an object ...
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW:L[0-9]+]]
// SPEC-BOXED: jne [[SLOW:L[0-9]+]]
```

and replace the block from `// SPEC: // Inline store with barrier
predicate` (line 120) through `// SPEC: mov byte ptr {{.*}}, 1` (line 125)
with:

```
// SPEC: // Inline store with barrier predicate
// SPEC: and {{.*}}, 0xFFFFFFFFFFC00000
// Under boxed doubles a young target jumps to the join, where the value is
// encoded once every declining check has run; a double with no inline
// encoding goes to the box arm rather than to the helper.
// SPEC-HV32: je [[JOIN:L[0-9]+]]{{$}}
// SPEC-HV32: {{^}}[[JOIN]]:
// SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
// SPEC-HV32: shl {{.*}}, 0x1D
// SPEC-HV32-NEXT: jnz [[BOX:L[0-9]+]]{{$}}
// SPEC-BOXED: je [[JOIN:L[0-9]+]]{{$}}
// SPEC-BOXED: {{^}}[[JOIN]]:
// SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
// SPEC-BOXED: shl {{.*}}, 0x3D
// SPEC-BOXED-NEXT: jnz [[BOX:L[0-9]+]]{{$}}
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32: mov dword ptr {{.*}}
// SPEC-BOXED: mov qword ptr {{.*}}
// The inline value's path ends in a jump to the card block it shares with
// the box arm, which follows at once (there is no counting block without
// -Xjit-emit-counters). The box arm bump-allocates the BoxedDouble -- a full
// young generation goes to the helper -- writes the double into it, tags
// the (under HV32, compressed) pointer, stores it, and dirties an old
// target's card unconditionally: a box is always young.
// SPEC-HV32: jmp [[DIRTY:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[BOX]]:
// SPEC-HV32-NEXT: // Box the double in the young generation
// SPEC-HV32-NEXT: mov [[CELL:r[0-9a-z]+]], qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32: ja [[SLOW]]{{$}}
// SPEC-HV32: mov qword ptr {{\[}}[[CELL]]+{{[0-9]+}}], {{r[0-9a-z]+}}
// SPEC-HV32-NEXT: sub [[CELL]], r15
// SPEC-HV32-NEXT: or [[CELL]], 4
// SPEC-HV32-NEXT: mov dword ptr {{.*}}, {{.*}}
// SPEC-HV32: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-HV32-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32-NEXT: je [[DONE:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
// SPEC-BOXED: jmp [[DIRTY:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[BOX]]:
// SPEC-BOXED-NEXT: // Box the double in the young generation
// SPEC-BOXED-NEXT: mov [[CELL:r[0-9a-z]+]], qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED: ja [[SLOW]]{{$}}
// SPEC-BOXED: mov qword ptr {{\[}}[[CELL]]+{{[0-9]+}}], {{r[0-9a-z]+}}
// SPEC-BOXED-NEXT: or [[CELL]], 4
// SPEC-BOXED-NEXT: mov qword ptr {{.*}}, [[CELL]]
// SPEC-BOXED: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-BOXED-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED-NEXT: je [[DONE:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
// SPEC: mov byte ptr {{.*}}, 1
// SPEC-HV32-NEXT: {{^}}[[DONE]]:
// SPEC-BOXED-NEXT: {{^}}[[DONE]]:
```

The lines active under HV64 are unchanged and in the same order.

In `test/jit/x86-64/putbyid-inline-emitted.js`, replace the end of the
header sentence at lines 29-30 (`so the value is` / `// encoded first --
the dispatch pinned below, whose one declining arm is a` / `// double that
would have to be boxed on the heap.`) with `so the value is` / `// encoded
-- the dispatch pinned below, whose number arm sends a double that does` /
`// not fit inline to the box arm.`, and the paragraph at lines 32-36
(from `// THE ENCODE COMES FIRST` through `// On Box2D two thirds of the
stores reaching these tiers decline.`) with:

```
// THE ENCODE FOLLOWS THE BARRIER PREDICATE'S DECLINING CHECKS, and the
// order below pins that: once a double that needs a box is boxed inline
// instead of declined, no encode outcome is a cheap early reject any more,
// and the box allocation must follow every check that can decline, so
// that a box is never allocated and abandoned (spec
// 2026-10-29-jit-inline-boxed-double-store-design.md).
```

Then replace everything from `// SPEC-LABEL:setIndirect:` (line 67) to
the end of the file with:

```
// SPEC-LABEL:setIndirect:
// SPEC: // Put to object specialization
//
// The guard comes first in every mode: the target must be an object, and
// its hidden class must be the one the write cache recorded, compared by
// lazy JIT id. It is the tier's first exit, so it is where the helper label
// gets its name.
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFF
// SPEC-HV64: jne [[SLOW:L[0-9]+]]
// SPEC-HV32: jne [[SLOW:L[0-9]+]]
// SPEC-BOXED: jne [[SLOW:L[0-9]+]]
// SPEC: movzx {{.*}}, word ptr {{.*}}
// SPEC: cmp {{.*}}
// SPEC: jne [[SLOW]]
//
// The barrier predicate. Every exit before the store goes to the helper
// without storing, or, for a young target, to the store.
// SPEC: // Inline store with barrier predicate
// SPEC: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC: je [[YOUNG:L[0-9]+]]
// SPEC: cmp byte ptr {{.*}}, 0
// SPEC: jne [[SLOW]]
// SPEC: cmp qword ptr {{.*}}, 1
// SPEC: jne [[SLOW]]
// SPEC: cmp word ptr {{.*}}, 1
// SPEC: jne [[SLOW]]
//
// Under boxed doubles the young exit lands on the join, after the last
// declining check, where the value is encoded into a SmallHermesValue,
// mirroring HermesValue32::encodeHermesValue(): the ETag selects between
// the inline ("compressed HV64") case, the three pointer tags and Symbol. A
// double whose low bits are not zero goes to the box arm; only a value with
// no SmallHermesValue encoding (RawHV32) still declines. Nothing at all is
// emitted for this in HV64, not even the comment.
// SPEC-HV32: {{^}}[[YOUNG]]:
// SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
// SPEC-HV32: sar {{.*}}, 0x2F
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFF6
// SPEC-HV32: ja [[NOTNUM:L[0-9]+]]
// The compression test: with a 29-bit payload, the low 35 bits must be zero.
// SPEC-HV32: shl {{.*}}, 0x1D
// SPEC-HV32-NEXT: jnz [[BOX:L[0-9]+]]{{$}}
// SPEC-HV32: shr {{.*}}, 0x20
// SPEC-HV32: {{^}}[[NOTNUM]]:
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFFA
// SPEC-HV32: jb [[SYM:L[0-9]+]]
// A pointer: the HermesValue tag biased into the HV32 one, or-ed into the
// compressed pointer.
// SPEC-HV32: sar {{.*}}, 1
// SPEC-HV32: add {{.*}}, 4
// SPEC-HV32: sub {{.*}}, r15
// SPEC-HV32: or {{.*}}
// SPEC-HV32: {{^}}[[SYM]]:
// SPEC-HV32: cmp {{.*}}, 0xFFFFFFFFFFFFFFF7
// SPEC-HV32: jne [[SLOW]]
// SPEC-HV32: shl {{.*}}, 3
// SPEC-HV32: or {{.*}}, 5
//
// The same dispatch in the boxed build without compressed pointers: a slot
// is a full eight bytes there, so the payload is 61 bits, the compression
// test is on the low three bits alone, the inline case needs no shift at
// all, and a pointer needs no compression either.
// SPEC-BOXED: {{^}}[[YOUNG]]:
// SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
// SPEC-BOXED: sar {{.*}}, 0x2F
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF6
// SPEC-BOXED: ja [[NOTNUM:L[0-9]+]]
// SPEC-BOXED: shl {{.*}}, 0x3D
// SPEC-BOXED-NEXT: jnz [[BOX:L[0-9]+]]{{$}}
// SPEC-BOXED: {{^}}[[NOTNUM]]:
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFFA
// SPEC-BOXED: jb [[SYM:L[0-9]+]]
// SPEC-BOXED: sar {{.*}}, 1
// SPEC-BOXED: add {{.*}}, 4
// SPEC-BOXED: or {{.*}}
// SPEC-BOXED: {{^}}[[SYM]]:
// SPEC-BOXED: cmp {{.*}}, 0xFFFFFFFFFFFFFFF7
// SPEC-BOXED: jne [[SLOW]]
// SPEC-BOXED: shl {{.*}}, 3
// SPEC-BOXED: or {{.*}}, 5
//
// The store itself: one slot wide, which is four bytes only under
// compressed pointers. Under boxed doubles a young target, which reached it
// through the join, is then done: no barrier for any value.
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32: mov dword ptr {{.*}}
// SPEC-HV32-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32-NEXT: je [[DONE:L[0-9]+]]{{$}}
// SPEC-BOXED: mov qword ptr {{.*}}
// SPEC-BOXED-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED-NEXT: je [[DONE:L[0-9]+]]{{$}}
//
// The card decision for an inline value, made on the original HermesValue
// in every mode, which is why its tag compare is the same everywhere: only
// an old target holding a pointer to a young cell needs a card.
// SPEC: sar {{.*}}, 0x30
// SPEC: cmp {{.*}}, 0xFFFFFFFFFFFFFFFD
//
// Under boxed doubles the inline path then jumps to the card block it
// shares with the box arm, which follows at once (no counting block without
// -Xjit-emit-counters). The box arm: bump-allocate (a full young generation
// goes to the helper), write the double, tag the pointer, store it into the
// same slot, and recompute the segment start (t1 was an allocation
// temporary): a young target is done, an old one always gets its card.
// SPEC-HV32: jmp [[DIRTY:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[BOX]]:
// SPEC-HV32-NEXT: // Box the double in the young generation
// SPEC-HV32-NEXT: mov [[CELL:r[0-9a-z]+]], qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32: ja [[SLOW]]{{$}}
// SPEC-HV32: mov qword ptr {{\[}}[[CELL]]+{{[0-9]+}}], {{r[0-9a-z]+}}
// SPEC-HV32-NEXT: sub [[CELL]], r15
// SPEC-HV32-NEXT: or [[CELL]], 4
// SPEC-HV32-NEXT: mov dword ptr {{.*}}, {{.*}}
// SPEC-HV32: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-HV32-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-HV32-NEXT: je [[DONE]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
// SPEC-BOXED: jmp [[DIRTY:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[BOX]]:
// SPEC-BOXED-NEXT: // Box the double in the young generation
// SPEC-BOXED-NEXT: mov [[CELL:r[0-9a-z]+]], qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED: ja [[SLOW]]{{$}}
// SPEC-BOXED: mov qword ptr {{\[}}[[CELL]]+{{[0-9]+}}], {{r[0-9a-z]+}}
// SPEC-BOXED-NEXT: or [[CELL]], 4
// SPEC-BOXED-NEXT: mov qword ptr {{.*}}, [[CELL]]
// SPEC-BOXED: and {{.*}}, 0xFFFFFFFFFFC00000
// SPEC-BOXED-NEXT: cmp {{.*}}, qword ptr [r15+{{[0-9]+}}]
// SPEC-BOXED-NEXT: je [[DONE]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
//
// The card-dirty store: the byte at segment start + (slot - segment start)
// >> 9.
// SPEC: shr {{.*}}, 9
// SPEC: mov byte ptr {{.*}}, 1
// In HV64 the young-target store follows the card block.
// SPEC-HV64: [[YOUNG]]:
// SPEC-HV64: mov qword ptr {{.*}}
// SPEC-HV32-NEXT: {{^}}[[DONE]]:
// SPEC-BOXED-NEXT: {{^}}[[DONE]]:
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
```

Spec resolution 1: two directives shared by all modes can no longer hold
in the boxed modes (the young label now precedes the store, the card block
falls through into `done`). `// SPEC: [[YOUNG]]:` becomes
`// SPEC-HV64: [[YOUNG]]:`; every other directive active under HV64 is
kept verbatim and in order, so the HV64 check sequence is unchanged.

- [ ] **Step 10: Build and see the tests fail**

```bash
for t in cmake-build-x86jit cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='store-boxed-double|inline-emitted' \
    cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: on `cmake-build-x86jit` (HV64) everything passes (the counters
exist and are 0, the HV64 pins are untouched). On `-hv32` and `-boxed`:
byval, byid and card FAIL with a box counter of 0; ygfull FAILS
(`NumPutByValBoxes: 0`, `NumStoreBoxFallbacks: 0`); values FAILS on its
DUMP RUN line only (no box arm); the two x86-64 pin files FAIL (the
encode still precedes the guards). Record each failure.

- [ ] **Step 11: The encoder takes boxLab**

In `lib/VM/JIT/x86-64/JitEmitter-internal.h`, replace the declaration
block from `#ifdef HERMESVM_BOXED_DOUBLES` (line 613) through `#endif`
(line 642) with:

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
/// Encode the HermesValue in \p in into the SmallHermesValue that a heap slot
/// holds, leaving the result in \p out. This is the inverse of
/// Emit_sh_shv_decode and mirrors HermesValue32::encodeHermesValue() case for
/// case, with one difference that is the whole reason it takes labels: a
/// double whose bits do not fit an inline "compressed HV64" has to be boxed
/// in a heap-allocated BoxedDouble, which this function does not allocate.
/// Such a value jumps to \p boxLab with nothing stored, where the caller
/// allocates the box. A value this code produces no encoding for -- a
/// RawHV32, or under HERMESVM_SANITIZE_HANDLES any number -- jumps to
/// \p slowLab, with nothing stored either.
///
/// Declared only under HERMESVM_BOXED_DOUBLES, unlike Emit_sh_shv_decode
/// which is always declared and emits nothing in the default build. The
/// decode can afford that because it works in place; an encode that produced
/// nothing would still owe its caller a register, so the caller selects
/// between this and using the HermesValue itself with the same #ifdef. Its
/// one caller is Emitter::emitSafeStoreOrSlow().
///
/// \param out receives the SmallHermesValue. Must differ from \p in.
/// \param in the HermesValue to encode; preserved.
/// \param temp scratch, clobbered. Must differ from \p in and \p out.
/// \param slowLab where to jump when the value has no encoding here.
/// \param boxLab where to jump with a double that needs a BoxedDouble.
///
/// EFLAGS are clobbered.
void emit_shv_encode_or_slow(
    x86::Assembler &a,
    const x86::Gp &out,
    const x86::Gp &in,
    const x86::Gp &temp,
    const asmjit::Label &slowLab,
    const asmjit::Label &boxLab);
#endif
```

and delete the wrapper `emit_shv_encode_for_slot_or_slow`, its doc
comment and the blank line after it (lines 644-676, from `/// Encode \p
value into the SmallHermesValue a heap slot holds, ready to be` through
its closing `}`).

In `lib/VM/JIT/x86-64/JitEmitter-internal.cpp`:
- change the encoder's signature (line 151-156) to take
  `const asmjit::Label &slowLab, const asmjit::Label &boxLab`;
- in its dispatch comment replace
  ```
  //   number or compressible -> a right shift, or a BoxedDouble allocation
  //                             this cannot do: decline;
  ```
  with
  ```
  //   number or compressible -> a right shift, or a BoxedDouble, which the
  //                             caller allocates: jump to boxLab;
  ```
- in the Handle-San arm replace `// for its side effect of moving the heap.
  Emitted code cannot allocate, so\n  // this whole case is declined to the
  helper, which does both.` with
  ```cpp
  // for its side effect of moving the heap. The caller's inline box
  // allocation would skip the young collection HadesGC::makeAImpl() forces
  // in that mode, so this whole case is declined to the helper, which does
  // both, and boxLab is never taken.
  ```
  and add `(void)boxLab;` on the line before `a.jmp(slowLab);`;
- in the other arm replace
  ```cpp
  // A double that does not fit has to become a heap-allocated BoxedDouble.
  // Nothing has been stored at this point, so declining is free and the
  // helper does the allocation.
  a.jnz(slowLab);
  ```
  with
  ```cpp
  // A double that does not fit has to become a heap-allocated BoxedDouble,
  // which the caller allocates at boxLab. Nothing has been stored yet.
  a.jnz(boxLab);
  ```

The symbol arm's `a.jne(slowLab);` (RawHV32) is unchanged.

- [ ] **Step 12: The box arm in emitSafeStoreOrSlow**

In `lib/VM/JIT/x86-64/JitEmitter-internal.cpp`, add
`#include "hermes/VM/BoxedDouble.h"` after `#include
"JitEmitter-internal.h"` (line 10).

Replace the head of the definition, from `#if HERMES_JIT_INLINE_SAFE_STORE`
(line 495) through `  asmjit::Label doneLab = a.newLabel();` (line
515), with:

```cpp
#if HERMES_JIT_INLINE_SAFE_STORE
#ifdef HERMESVM_BOXED_DOUBLES
void Emitter::emitSafeStoreOrSlow(
    const x86::Gp &loc,
    const x86::Gp &value,
    const x86::Gp &t1,
    const x86::Gp &t2,
    const x86::Gp &t3,
    JitCounter boxCounter,
    const asmjit::Label &slowLab) {
  assert(t1 != loc && t1 != value && "t1 must not alias loc or value");
  assert(t2 != loc && t2 != value && "t2 must not alias loc or value");
  assert(t3 != loc && t3 != value && "t3 must not alias loc or value");
  assert(t1 != t2 && t1 != t3 && t2 != t3 && "the temporaries must differ");
#else
void Emitter::emitSafeStoreOrSlow(
    const x86::Gp &loc,
    const x86::Gp &shv,
    const x86::Gp &value,
    const x86::Gp &t1,
    const x86::Gp &t2,
    const asmjit::Label &slowLab) {
  assert(t1 != loc && t1 != value && "t1 must not alias loc or value");
  assert(t2 != loc && t2 != value && "t2 must not alias loc or value");
  assert(t1 != t2 && "the two temporaries must differ");
  assert(
      shv != loc && shv != t1 && "the encoded value must survive to the store");
  // In the default heap-value mode a slot holds the HermesValue itself, so
  // the tiers pass \p value as \p shv.
#endif

  comment("// Inline store with barrier predicate");

#ifdef HERMESVM_BOXED_DOUBLES
  // A young target skips the barrier checks but not the encode: it jumps to
  // the join, where every path that stores begins.
  asmjit::Label joinLab = a.newLabel();
  const asmjit::Label &youngTargetLab = joinLab;
  asmjit::Label boxLab = a.newLabel();
  asmjit::Label dirtyCardLab = a.newLabel();
  // With -Xjit-emit-counters a full young generation goes through an
  // out-of-line block that counts it; otherwise straight to the helper.
  asmjit::Label slowFullLab = emitCounters_ ? a.newLabel() : slowLab;
#else
  asmjit::Label youngTargetLab = a.newLabel();
#endif
  asmjit::Label doneLab = a.newLabel();
```

(The default-mode arm creates its two labels in today's order, so HV64
label numbers do not move.) Keep lines 517-571 (from `  // The mask that
turns a pointer into the start of its segment.` through the segment-size
test's `  a.jne(slowLab);`) unchanged. Replace the rest of the definition,
from `  // The barrier is now known to be at most a card-dirty. Store.`
(line 573) through `#endif // HERMES_JIT_INLINE_SAFE_STORE` (line 613),
with:

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  // The join. Live: loc, value and t1, the segment start; t2 and t3 are
  // free, and nothing is live in a vector register or xScratch, which is
  // what allocInYoung() below needs under ASan. The structural guards and
  // the barrier checks have run; the declines left are the encode's RawHV32
  // and Handle-San rejections, inside it, and the box arm's full young
  // generation, which branches from the bump before the new level is
  // stored. Each precedes the slot store and the publication of a box.
  a.bind(joinLab);
  emit_shv_encode_or_slow(a, t2, value, t3, slowLab, boxLab);
  emit_store_shv(a, t2, x86::ptr(loc));
  // HadesGC::writeBarrier() step (1) again: a young target needs no barrier
  // for any value. The compare is repeated because the encode clobbered
  // EFLAGS.
  a.cmp(t1, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.je(doneLab);
#else
  // The barrier is now known to be at most a card-dirty. Store.
  emit_store_shv(a, shv, x86::ptr(loc));
#endif

  // relocationWriteBarrier() is only reached for a pointer value. The test is
  // made on the ORIGINAL HermesValue rather than on what was just stored,
  // which is both cheaper and more accurate: a HermesValue carries its
  // pointer uncompressed, which is what the segment compare below needs. The
  // one pointer a SmallHermesValue has that a HermesValue does not -- a
  // BoxedDouble -- never reaches this test: the box arm below stores it. \p
  // t2 is dead once the store above has consumed it.
  emit_sh_ljs_get_tag(a, t2, value);
  emit_sh_ljs_tag_is_pointer(a, t2);
  a.jb(doneLab);

  // t2 = the start of the segment containing the pointed-to cell. Only an
  // old-to-young pointer needs a card; old-to-old does not, and the compactee
  // cases were declined above.
  emit_sh_ljs_get_pointer(a, t2, value);
  a.and_(t2, asmjit::Imm(kSegmentMask));
  a.cmp(t2, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.jne(doneLab);

#ifdef HERMESVM_BOXED_DOUBLES
  a.jmp(dirtyCardLab);

  if (emitCounters_) {
    // Out of line: doneLab falls through into the tier's jump to its
    // continuation, so nothing may sit between them.
    a.bind(slowFullLab);
    comment("// Young generation full: count the fallback");
    emitIncrementCounter(JitCounter::NumStoreBoxFallbacks);
    a.jmp(slowLab);
  }

  // The box arm, HermesValue32::encodeHermesValue()'s BoxedDouble::create()
  // inline: bump-allocate the box in the young generation (t2 the cell, t1
  // and t3 its temporaries), write the double into it, and store the tagged
  // compressed pointer. A full young generation jumps before the new level
  // is published, so a box is never allocated and abandoned. Nothing here
  // is a GC safepoint: in ASan builds the bump calls
  // __asan_unpoison_memory_region with the GP temps saved, and that is the
  // only call.
  a.bind(boxLab);
  comment("// Box the double in the young generation");
  allocInYoung(
      CellKind::BoxedDoubleKind,
      cellSize<BoxedDouble>(),
      t2,
      t1,
      t3,
      slowFullLab);
  static_assert(
      RuntimeOffsets::boxedDoubleValue <= (uint32_t)INT32_MAX,
      "the double's offset in a BoxedDouble must fit a disp32");
  a.mov(x86::qword_ptr(t2, (int32_t)RuntimeOffsets::boxedDoubleValue), value);
  // encodePointerImpl(box, Tag::BoxedDouble): the compressed pointer with the
  // tag in its low bits, which heap alignment leaves clear.
  emit_sh_cp_encode_non_null(a, t2);
  static_assert(
      (uint32_t)HermesValue32::Tag::BoxedDouble <
          ((uint32_t)1 << RuntimeOffsets::kShvTagBits),
      "the BoxedDouble tag must fit the bits heap alignment leaves clear");
  a.or_(t2, asmjit::Imm((uint32_t)HermesValue32::Tag::BoxedDouble));
  emit_store_shv(a, t2, x86::ptr(loc));
  emitIncrementCounter(boxCounter);
  // t1 was an allocation temporary: recompute the segment start. A young
  // target is done; an old one always needs its card, because the box is
  // young by construction.
  a.mov(t1, loc);
  a.and_(t1, asmjit::Imm(kSegmentMask));
  a.cmp(t1, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.je(doneLab);

  a.bind(dirtyCardLab);
#endif

  // FixedSizeHeapSegment::dirtyCardForAddress(loc), inline:
  //   segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  // The card array base is the segment start itself, since
  // RuntimeOffsets::segmentInlineCards is 0. The runtime writes this byte
  // with a relaxed atomic store, which on x86 is this same instruction.
  a.mov(t2, loc);
  a.sub(t2, t1);
  a.shr(t2, asmjit::Imm(RuntimeOffsets::kLogCardSize));
  a.mov(
      x86::byte_ptr(t1, t2, 0, (int32_t)RuntimeOffsets::segmentInlineCards),
      asmjit::Imm(RuntimeOffsets::kCardDirty));
#ifndef HERMESVM_BOXED_DOUBLES
  a.jmp(doneLab);

  a.bind(youngTargetLab);
  emit_store_shv(a, shv, x86::ptr(loc));
#endif

  a.bind(doneLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE
```

Default mode, instruction for instruction: `and`, `cmp/je young`, the
three checks, `store shv`, the tag and segment tests, the card, `jmp
done`, `young: store shv`, `done` -- today's sequence.

- [ ] **Step 13: The declaration and its docs**

In `lib/VM/JIT/x86-64/JitEmitter.h`, replace lines 1626-1668 (from `  ///
Emit an inline store of the already-encoded slot value in \p shv to the`
through `  /// requirement holds by construction rather than by convention.`)
with:

```cpp
  /// Emit an inline store of \p value to the heap slot whose address is in
  /// \p loc, performed only when the Hades write barrier for that store is
  /// provably either a no-op or a single card-dirty. In every other case --
  /// concurrent marking active, a compaction in progress, or a segment whose
  /// card array is not the inline one -- nothing is stored and control jumps
  /// to \p slowLab, whose code is expected to perform the store through the
  /// runtime, barrier included.
  ///
  /// A heap slot holds a SmallHermesValue, not a HermesValue, and under
  /// HERMESVM_BOXED_DOUBLES the two differ. There this function encodes
  /// \p value itself, with emit_shv_encode_or_slow(), after the structural
  /// and barrier checks, and stores a double the encoding cannot hold inline
  /// through a box arm: it bump-allocates a BoxedDouble in the young
  /// generation, writes the double into it, and stores the tagged
  /// compressed pointer. The box arm's only decline is a full young
  /// generation, taken before anything is published or stored, through an
  /// out-of-line block that counts it (JitCounter::NumStoreBoxFallbacks)
  /// under -Xjit-emit-counters. In the default heap-value mode a slot holds
  /// the HermesValue itself, nothing is encoded, and the caller passes
  /// \p value as \p shv.
  ///
  /// The card decision for an inline-encoded value is phrased in terms of
  /// the ORIGINAL 64-bit \p value: the two agree on which values are
  /// pointers, and a HermesValue carries its pointer uncompressed, which is
  /// what the segment compare needs. A box is young by construction, so the
  /// box arm dirties an old target's card unconditionally.
  ///
  /// The emitted predicate mirrors HadesGC::writeBarrier() and
  /// HadesGC::relocationWriteBarrier() exactly:
  ///
  ///     segLoc = loc & ~(kSegmentUnitSize-1)
  ///     if (segLoc == runtime.heap_.youngGen_.lowLim_)  // young target:
  ///       goto store                                    //   no barrier
  ///     if (runtime.heap_.ogMarkingBarriers_)  goto slow  // snapshot barrier
  ///     if (compactee active)                  goto slow  // relocation into
  ///     if (segLoc's size != 1 unit)           goto slow  //   the compactee
  ///   store:
  ///     shv = encode(value)            // boxed doubles; a box: goto box
  ///     *loc = shv
  ///     if (segLoc == youngGen lowLim)  goto done         // boxed doubles
  ///     if (value is a pointer &&
  ///         (value.ptr & ~(kSegmentUnitSize-1)) == youngGen lowLim)
  ///       segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  ///     goto done
  ///   box:                             // boxed doubles only
  ///     cell = bump-allocate a BoxedDouble    // young gen full: goto slow
  ///     cell->value_ = value
  ///     *loc = compress(cell) | Tag::BoxedDouble
  ///     if (segLoc != youngGen lowLim)
  ///       segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  ///   done:
  ///
  /// The order matters: the snapshot barrier reads the OLD contents of the
  /// slot, so the marking test has to precede the store. It does, and the
  /// path that stores is precisely the path on which marking is off, so the
  /// requirement holds by construction rather than by convention. Every
  /// decline also precedes the slot store and the publication of a
  /// successful box allocation: the structural and barrier checks precede
  /// the encode, the encode's own rejection precedes the box arm, and a full
  /// young generation branches from the bump before the new level is
  /// stored. So a box is never allocated and then abandoned.
```

Keep lines 1669-1695 (`  ///` and the PRECONDITION, asymmetry and
segment-size paragraphs) unchanged. Replace lines 1696-1717 (from the
`  ///` before `  /// \param loc address of the slot; preserved.` through
the declaration's `      const asmjit::Label &slowLab);`) with:

```cpp
  ///
  /// \param loc address of the slot; preserved.
  /// \param shv (default heap-value mode only) the value to store, which is
  ///   \p value: there a slot holds the HermesValue itself.
  /// \param value the original 64-bit HermesValue; preserved.
  /// \param t1 scratch, clobbered. Must differ from \p loc and \p value.
  /// \param t2 scratch, clobbered. Must differ from \p loc and \p value.
  /// \param t3 (boxed doubles only) scratch, clobbered: the encode's
  ///   temporary and the box allocation's second one. Must differ from
  ///   \p loc, \p value, \p t1 and \p t2.
  /// \param boxCounter (boxed doubles only) the counter the box arm
  ///   increments after its store under -Xjit-emit-counters.
  /// \param slowLab where to jump when the store was NOT performed.
  ///
  /// EFLAGS are clobbered. In the default heap-value mode nothing else is
  /// touched and no call is emitted, so no register needs to be synced or
  /// freed around it. Under boxed doubles the same holds except in ASan
  /// builds, where the box allocation calls __asan_unpoison_memory_region
  /// with the eight GP temps saved around it (bumpAllocAndUnpoison()), and
  /// with -Xjit-emit-counters, whose counting blocks write xScratch: no live
  /// value may be held in a vector temp or in xScratch across this function.
#ifdef HERMESVM_BOXED_DOUBLES
  void emitSafeStoreOrSlow(
      const x86::Gp &loc,
      const x86::Gp &value,
      const x86::Gp &t1,
      const x86::Gp &t2,
      const x86::Gp &t3,
      JitCounter boxCounter,
      const asmjit::Label &slowLab);
#else
  void emitSafeStoreOrSlow(
      const x86::Gp &loc,
      const x86::Gp &shv,
      const x86::Gp &value,
      const x86::Gp &t1,
      const x86::Gp &t2,
      const asmjit::Label &slowLab);
#endif
```

- [ ] **Step 14: The tiers**

In `lib/VM/JIT/x86-64/JitEmitter-property.cpp`, `emitPutByValFastArrayTier`:

After `  HWReg hwTemp2 = allocTempGpX();` (line 61) add

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  // emitSafeStoreOrSlow()'s third temporary, for the encode and the box arm.
  // Allocated last, so the registers above are the default mode's.
  HWReg hwTemp3 = allocTempGpX();
#endif
```

after `  const x86::Gp temp2 = hwTemp2.gpq();` (line 70) add

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  const x86::Gp temp3 = hwTemp3.gpq();
#endif
```

after `  freeReg(hwTemp2);` (line 75) add

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  freeReg(hwTemp3);
#endif
```

delete the encode block (lines 83-93: the comment `  // The value has to
be encoded into the SmallHermesValue an element slot` through `
emit_shv_encode_for_slot_or_slow(a, temp2, value, temp1, helperLab);` and
the blank line after it), and replace the final call (line 201-202)

```cpp
  // Store, if the write barrier for it is a no-op or a card-dirty.
  emitSafeStoreOrSlow(loc, shv, value, temp1, temp2, helperLab);
```

with

```cpp
  // Store, if the write barrier for it is a no-op or a card-dirty. Under
  // boxed doubles this also encodes the value, boxing a double that needs
  // it.
#ifdef HERMESVM_BOXED_DOUBLES
  emitSafeStoreOrSlow(
      loc, value, temp1, temp2, temp3, JitCounter::NumPutByValBoxes, helperLab);
#else
  emitSafeStoreOrSlow(loc, value, value, temp1, temp2, helperLab);
#endif
```

`emitPutByIdInlineTier`: the same three insertions after `  HWReg hwTemp2
= allocTempGpX();` (line 2351), `  const x86::Gp temp2 = hwTemp2.gpq();`
(line 2356) and `  freeReg(hwTemp2);` (line 2359); delete the encode block
(lines 2365-2375, the comment `  // The value has to be encoded into the
SmallHermesValue a property slot` through the call and its blank line);
and replace the final call (lines 2409-2410) with

```cpp
  // Store, if the write barrier for it is a no-op or a card-dirty. Under
  // boxed doubles this also encodes the value, boxing a double that needs
  // it.
#ifdef HERMESVM_BOXED_DOUBLES
  emitSafeStoreOrSlow(
      loc, value, temp1, temp2, temp3, JitCounter::NumPutByIdBoxes, helperLab);
#else
  emitSafeStoreOrSlow(loc, value, value, temp1, temp2, helperLab);
#endif
```

Budget (spec "Join-point contract"): the PutByVal tier now holds `target`,
`value`, `loc`, `idx`, `temp1`, `temp2`, `temp3`: at most 7 of the 8 GP
temps (x86-64/JitEmitter.h:200-216), the PutById tier at most 6, so no
allocation spills.

In the GetByVal tier comment (lines 1615-1622) replace `this is the load
side's mirror of emit_shv_encode_for_slot_or_slow(), and unlike that
encode it never has anything to decline:` with `this is the load side's
mirror of emit_shv_encode_or_slow(), and unlike that encode it never has
anything to decline or to box:` (re-wrap to 80 columns).

`grep -n 'emit_shv_encode_for_slot_or_slow' -r lib/VM/JIT/x86-64` must now
print nothing.

- [ ] **Step 15: Build and run**

```bash
for t in cmake-build-x86jit cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='store-boxed-double|inline-emitted|inline-store-shv|putbyval-inline|putbyid-inline' \
    cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: all pass on all three trees. If a pin fails only on the text of
an operand (`0x4` for `4`, a register class), read the dump
(`cmake-build-x86jit-hv32/bin/hermes -fno-inline -Xjit=force
-Xdump-jitcode=3 test/jit/x86-64/putbyval-inline-emitted.js`, and
`-Xjit -Xjit-threshold=4` for putbyid) and fix the regex to the dump's
text, keeping the instruction, the captured registers and labels, and the
`-NEXT` adjacency; never drop a pin or weaken a capture. If a count
differs, find why (`-Xdump-jitcode=3` shows the bodies;
`-gc-print-collection-stats` the collections) and fix the code or the
derivation, never just the number.

- [ ] **Step 16: HV64 emitted code unchanged**

```bash
$S/hv32impl/hv64-ident.sh $S/hv32impl/base/hermes-x86jit \
  cmake-build-x86jit/bin/hermes $S/hv32impl/ident-task1-x86
```

Expected: six `IDENTICAL` lines, exit 0; the `moved` count may be
nonzero (the binary changed), `differences` must be 0. A difference is a
defect in an `#ifdef` arm: fix it before continuing.

- [ ] **Step 17: The GC conditions, once per tree**

The counts are the direct evidence (Test conventions); this step checks
the conditions behind their derivation. `-gc-print-collection-stats`
prints, after the summary, one JSON record per collection in completion
order, with its `collectionType`, `cause` (`forced` for gc(), `natural`
otherwise), `tags`, and allocated and external bytes before and after. A
young collection that starts an old-generation collection is tagged `old
gen start` when it starts it (HadesGC.cpp:1562), so a start that never
completes -- the case the spec allows at the end of gc()
(HadesGC.cpp:1475-1481), which the completed-collection totals cannot show
(`numOldCollections_` counts at sweep end, HadesGC.cpp:1717-1726) -- is
still visible. Checked by hand on this tree: two gc() calls print, in
order, `young forced [old gen start]`, `waiting forced`, `old forced`,
`young forced`, twice; a churn after them adds `young natural` records.
`gc-check.py` (Step 1) takes the forced prefix (every record before the
first natural one: the measured stores lie between its end and the first
natural record) and requires exactly NGC old-generation starts, NGC
completed old collections and 2 NGC young collections in it, no start in a
gc()'s second young collection, no natural collection before it (none at
all except in the card test), and the peak of allocated plus external
bytes over its records, before and after each collection, under 7 MiB
(spec condition 1; after a gc() the young generation is empty, so this is
the old generation's). There is no stats field that reads the marking
flag itself; the start tags, each matched by a completed old collection
inside the forced prefix, are the direct observation that marking is off
and no compactee is pending when gc() returns (the second young collection
of each gc() evacuates any compactee, HadesGC.cpp:1475-1481; a `compact`
tag on it shows one was).

```bash
for t in cmake-build-x86jit cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  H="$t/bin/hermes -fno-inline -Xjit-crash-on-error -Xhermes-internal-test-methods -gc-print-collection-stats"
  for m in young old; do
    n=1; [ $m = old ] && n=2
    echo "== $t byval $m"
    $H -Xjit=force test/jit/store-boxed-double-byval.js -- $m 2>&1 |
      python3 -I $S/hv32impl/gc-check.py $n
    echo "== $t byid $m"
    $H -Xjit -Xjit-threshold=4 test/jit/store-boxed-double-byid.js -- $m 2>&1 |
      python3 -I $S/hv32impl/gc-check.py $n
  done
  for m in byval byid byidwide; do
    echo "== $t card $m"
    $H -Xjit -Xjit-threshold=4 test/jit/store-boxed-double-card.js -- $m 2>&1 |
      python3 -I $S/hv32impl/gc-check.py 2 --natural-after
  done
done
```

Expected: every run exits 0 with `peak allocated+external` far below 7
MiB (a few hundred KB; checked by hand: about 220 KB for a small script
with two gc() calls) and, for the card cases, at least one natural young
collection after the forced prefix. Record each summary line. A failure
means the derivation of a count rests on a false premise even if the test
passed: investigate and report; never adjust the test to it.

- [ ] **Step 18: Prove the tests can fail**

On `cmake-build-x86jit-hv32` unless stated; one mutation at a time in
`lib/VM/JIT/x86-64/JitEmitter-internal.cpp`, `emitSafeStoreOrSlow`;
rebuild after each change and each restore; run
`LIT_FILTER='store-boxed-double|inline-emitted'`:
- Mutation A: `emit_shv_encode_or_slow(a, t2, value, t3, slowLab,
  boxLab);` -> `... slowLab, slowLab);` (every box declines). Expected
  FAIL: byval, byid, card (`...Boxes: 0`), ygfull (`NumPutByValBoxes: 0`),
  both x86-64 pin files. Repeat on `cmake-build-x86jit-boxed`: the same.
- Mutation B: in the box arm, the `a.je(doneLab);` after the recomputed
  young compare -> `a.jmp(doneLab);` (no card for an old target). Expected
  FAIL: all three card RUN lines (an assertion in
  `HadesGC::verifyCardTable()` or `value false`), and the pin files.
  Repeat on `-boxed`. Also note whether ygfull failed (it should, through
  `verifyCardTable()` at its second young collection).
- Mutation C: delete the box arm's `emit_sh_cp_encode_non_null(a, t2);`.
  Expected FAIL on `-hv32`: byval, byid, values (wrong values or an ASan
  report). On `-boxed` the encode emits nothing, so nothing fails: say so.
- Mutation D: `a.mov(x86::qword_ptr(t2, (int32_t)RuntimeOffsets::
  boxedDoubleValue), value);` -> `a.mov(x86::dword_ptr(t2, (int32_t)
  RuntimeOffsets::boxedDoubleValue), value.r32());` (half the double).
  Expected FAIL: byval and byid (`mismatches 1000`), values (diff against
  the interpreter), on `-hv32` and `-boxed`.
- Mutation E: in the counting block, `a.jmp(slowLab);` -> `a.jmp(doneLab);`
  (the fallback store is lost). Expected FAIL: ygfull's first RUN line
  (`mismatches` nonzero); its second RUN line (no counters, no block)
  passes: say so.
- Mutation F: delete `emitIncrementCounter(JitCounter::NumStoreBoxFallbacks);`.
  Expected FAIL: ygfull's first RUN line (`NumStoreBoxFallbacks: 0`).

After the last restore, rebuild all three trees and re-run Step 15's
command: all pass.

- [ ] **Step 19: Suites**

```bash
for t in cmake-build-x86jit cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target check-hermes || echo "FAIL $t"
done
cmake --build cmake-build-host --target hermesc shermes
cmake --build cmake-build-arm64 --target hermes
```

Expected: no failure; the arm64 tree builds (the shared counters header
changed; the arm64 emitter did not). `putbyval-inline.js`,
`putbyid-inline.js` and `inline-store-shv-shapes.js` pass unchanged
(spec test 6). An existing counter test whose count moved under HV32 or
BOXED only, because a box store no longer declines (fewer declines, fewer
`NumRecompileChecks`, `NumRecompiles`, `NumByValDemotions`): derive the new
value in the test's comment, use `%hv-mode` prefixes if the modes now
differ, and list the test in the report. Anything else is a bug.

- [ ] **Step 20: Commit**

```bash
git clang-format HEAD -- include/hermes/VM/JIT/JitCounters.h \
  lib/VM/JIT/x86-64/JitEmitter-internal.h \
  lib/VM/JIT/x86-64/JitEmitter-internal.cpp \
  lib/VM/JIT/x86-64/JitEmitter.h lib/VM/JIT/x86-64/JitEmitter-property.cpp
git add include/hermes/VM/JIT/JitCounters.h \
  lib/VM/JIT/x86-64/JitEmitter-internal.h \
  lib/VM/JIT/x86-64/JitEmitter-internal.cpp \
  lib/VM/JIT/x86-64/JitEmitter.h lib/VM/JIT/x86-64/JitEmitter-property.cpp \
  test/jit/x86-64/putbyval-inline-emitted.js \
  test/jit/x86-64/putbyid-inline-emitted.js \
  test/jit/store-boxed-double-byval.js test/jit/store-boxed-double-byid.js \
  test/jit/store-boxed-double-card.js test/jit/store-boxed-double-ygfull.js \
  test/jit/store-boxed-double-values.js
git commit   # "JIT: box doubles inline in the x86-64 store tiers"
```

(Add any existing test Step 19 reconciled, by path.)

---

### Task 2: arm64 inline boxing

**Files:**
- Modify: `lib/VM/JIT/arm64/JitEmitter-internal.h:716-778`
- Modify: `lib/VM/JIT/arm64/JitEmitter-internal.cpp:8-14` (include),
  `:118-273` (encoder), `:487-631` (`emitSafeStoreOrSlow`)
- Modify: `lib/VM/JIT/arm64/JitEmitter.h:1560-1602`, `:1630-1655`,
  `:2090-2128` (`bumpAllocAndUnpoison`, `allocInYoung` docs)
- Modify: `lib/VM/JIT/arm64/JitEmitter-property.cpp:54-96`, `:124-128`,
  `:249` (PutByVal tier), `:1716-1722` (GetByVal comment), `:2528-2553`,
  `:2566`, `:2591` (PutById tier)
- Modify: `test/jit/putbyval-inline-emitted-arm64.js:41-44, 96-111, 194-199`,
  `test/jit/putbyid-inline-emitted-arm64.js:31-40, 65-207`
- Modify: the five `test/jit/store-boxed-double-*.js` (lift the gate)

**Interfaces:**
- Consumes: Task 1's counters and tests; arm64 `allocInYoung(CellKind,
  uint32_t, const a64::GpX &xOut, const a64::GpX &xTemp1, const a64::GpX
  &xTemp2, const asmjit::Label &)` (arm64/JitEmitter-alloc.cpp:97-108;
  `initGCCell` clobbers `xTemp1`; the bump's `add/cmp/b.hi` at :28-34
  precede the level store at :54); `emitIncrementCounter` (arm64
  JitEmitter.cpp:656-670: saves and restores x0/x1, writes no flags);
  `emit_sh_cp_encode_non_null` (arm64/JitEmitter-internal.h:515-521);
  `emit_store_shv` (:563); `maxNaturalBaseOffset` (:315);
  `a64::Utils::isLogicalImm`.
- Produces: the arm64 signatures of Global Constraints.

Register contract: Task 1's, with these arm64 facts. Every runtime word
the predicate tests is loaded into `xScratch` (x16) and consumed by the
next instruction, so nothing is held in it at `boxLab`; the vector temps
are dead (the PutByVal key's last use is the index guard,
arm64/JitEmitter-property.cpp:148-153). The arm64 ASan save loop pushes
x0-x15 (arm64/JitEmitter-alloc.cpp:36-51); the arm64 trees are built
without ASan, so that path is not exercised locally (spec "Not tested").

- [ ] **Step 1: Invoke the gc-safe-coding skill.**

- [ ] **Step 2: Lift the gates and update the arm64 pins; see them fail**

In each of the five `test/jit/store-boxed-double-*.js`, replace the two
lines

```
// REQUIRES: jit, jit-arch-x86-64
// x86-64 only until the arm64 emitter boxes inline (plan Task 2).
```

with `// REQUIRES: jit`.

In `test/jit/putbyval-inline-emitted-arm64.js`:
- replace the header sentences (lines 41-44) `On top of those, the two
  boxed modes encode the value into a SmallHermesValue before any guard
  runs; the shape of that dispatch is pinned by
  putbyid-inline-emitted-arm64.js and only its comment line is checked
  here.` with `On top of those, the two boxed modes encode the value inside
  the barrier predicate and box a double that needs it; the encode's
  dispatch is pinned by putbyid-inline-emitted-arm64.js, its position and
  the box arm here too.` (re-wrapped);
- replace lines 96-103 (from `// Under boxed doubles the value is encoded
  into a SmallHermesValue first,` through `// SPEC-BOXED: b.ne
  [[SLOW:L[0-9]+]]` and the `//` after it) with
  ```
  // In every mode the object check below is the tier's first exit, so it is
  // where the helper label gets its name: the value is encoded inside the
  // barrier predicate, not ahead of the guards.
  //
  ```
- replace the object-check lines `// SPEC-HV32: b.ne [[SLOW]]` and
  `// SPEC-BOXED: b.ne [[SLOW]]` (lines 110-111) with
  `// SPEC-HV32: b.ne [[SLOW:L[0-9]+]]` and
  `// SPEC-BOXED: b.ne [[SLOW:L[0-9]+]]`;
- in the flags comment replace `first -- in x16, because the other
  temporary is carrying the encoded value in the two boxed modes.` with
  `first -- in x16, the backend's non-allocated scratch.`;
- replace the block from `// SPEC: and {{x[0-9]+}}, {{x[0-9]+}},
  0xFFFFFFFFFFC00000` (line 194) through `// SPEC: strb w16,
  {{\[}}{{x[0-9]+}}, {{x[0-9]+}}]` (line 199) with:
  ```
  // SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
  // Under boxed doubles a young target jumps to the join, where the value is
  // encoded once every declining check has run; a double with no inline
  // encoding goes to the box arm rather than to the helper.
  // SPEC-HV32: b.eq [[JOIN:L[0-9]+]]{{$}}
  // SPEC-HV32: {{^}}[[JOIN]]:
  // SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
  // SPEC-HV32: tst {{x[0-9]+}}, 0x7FFFFFFFF
  // SPEC-HV32-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
  // SPEC-BOXED: b.eq [[JOIN:L[0-9]+]]{{$}}
  // SPEC-BOXED: {{^}}[[JOIN]]:
  // SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
  // SPEC-BOXED: tst {{x[0-9]+}}, 7
  // SPEC-BOXED-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
  // SPEC-HV64: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
  // SPEC-HV32: str {{w[0-9]+}}, {{\[}}{{x[0-9]+}}]
  // SPEC-BOXED: str {{x[0-9]+}}, {{\[}}{{x[0-9]+}}]
  // The inline value's path ends in a jump to the card block it shares with
  // the box arm, which follows at once (no counting block without
  // -Xjit-emit-counters). The box arm bump-allocates the BoxedDouble -- a
  // full young generation goes to the helper -- writes the double, tags the
  // (under HV32, compressed) pointer, stores it, and dirties an old target's
  // card unconditionally: a box is always young.
  // SPEC-HV32: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
  // SPEC-HV32-NEXT: {{^}}[[BOX]]:
  // SPEC-HV32-NEXT: // Box the double in the young generation
  // SPEC-HV32-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
  // SPEC-HV32: b.hi [[SLOW]]{{$}}
  // SPEC-HV32: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
  // SPEC-HV32-NEXT: sub [[CELL]], [[CELL]], x19
  // SPEC-HV32-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
  // SPEC-HV32-NEXT: str {{w[0-9]+}}, {{\[}}{{x[0-9]+}}]
  // SPEC-HV32-NEXT: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
  // SPEC-HV32-NEXT: ldr x16, [x19, {{[0-9]+}}]
  // SPEC-HV32-NEXT: cmp {{x[0-9]+}}, x16
  // SPEC-HV32-NEXT: b.eq [[DONE:L[0-9]+]]{{$}}
  // SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
  // SPEC-BOXED: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
  // SPEC-BOXED-NEXT: {{^}}[[BOX]]:
  // SPEC-BOXED-NEXT: // Box the double in the young generation
  // SPEC-BOXED-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
  // SPEC-BOXED: b.hi [[SLOW]]{{$}}
  // SPEC-BOXED: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
  // SPEC-BOXED-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
  // SPEC-BOXED-NEXT: str [[CELL]], {{\[}}{{x[0-9]+}}]
  // SPEC-BOXED-NEXT: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
  // SPEC-BOXED-NEXT: ldr x16, [x19, {{[0-9]+}}]
  // SPEC-BOXED-NEXT: cmp {{x[0-9]+}}, x16
  // SPEC-BOXED-NEXT: b.eq [[DONE:L[0-9]+]]{{$}}
  // SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
  // SPEC: mov w16, 1
  // SPEC: strb w16, {{\[}}{{x[0-9]+}}, {{x[0-9]+}}]
  // SPEC-HV32-NEXT: {{^}}[[DONE]]:
  // SPEC-BOXED-NEXT: {{^}}[[DONE]]:
  ```

In `test/jit/putbyid-inline-emitted-arm64.js` replace, in the header
sentence at lines 31-32, `value is encoded first -- the dispatch pinned
below, whose one declining` / `// arm is a double that would have to be
boxed on the heap.` with `value is encoded -- the dispatch pinned below,
whose number arm sends a` / `// double that does not fit inline to the box
arm.` (re-wrap through `pointers.`), replace the paragraph at lines 36-40
(`// THE ENCODE COMES FIRST` through `// On Box2D two thirds of the stores
reaching these tiers decline.`) with the paragraph Task 1 Step 9 put in the
x86-64 file, and replace everything from
`// SPEC-LABEL:setIndirect:` (line 65) to the end of the file with:

```
// SPEC-LABEL:setIndirect:
// SPEC: // Put to object specialization
//
// The guard comes first in every mode: the target must be an object, and its
// hidden class must be the one the write cache recorded, compared by lazy JIT
// id. arm64 tests a tag by shifting it down and adding, so "is an object" is
// `asr`+`cmn` rather than x86-64's `sar`+`cmp`. It is the tier's first exit,
// so it is where the helper label gets its name.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 1
// SPEC-HV64: b.ne [[SLOW:L[0-9]+]]
// SPEC-HV32: b.ne [[SLOW:L[0-9]+]]
// SPEC-BOXED: b.ne [[SLOW:L[0-9]+]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC: ldrh {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: cmp {{w[0-9]+}}, {{[0-9]+}}
// SPEC: b.ne [[SLOW]]
//
// An indirect slot: load the property storage, then add the element's byte
// offset within it. A direct slot is the same `add` without the load. The
// load is a compressed-pointer load, four bytes wide under compressed
// pointers and eight otherwise.
// SPEC-HV64: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-HV32: ldr {{w[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC-BOXED: ldr {{x[0-9]+}}, [{{x[0-9]+}}, {{[0-9]+}}]
// SPEC: add {{x[0-9]+}}, {{x[0-9]+}}, {{0x[0-9A-F]+}}
//
// The barrier predicate. Every exit before the store goes to the helper
// without storing, or, for a young target, to the store. Unlike x86-64,
// which compares against memory, each runtime word tested here is loaded
// into x16, the backend's non-allocated scratch, first: one temporary holds
// the segment start, and the other two belong to the encode and the box
// arm.
// SPEC: // Inline store with barrier predicate
// SPEC: and [[SEG:x[0-9]+]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp [[SEG]], x16
// SPEC: b.eq [[YOUNG:L[0-9]+]]
// SPEC: ldrb w16, [x19, {{[0-9]+}}]
// SPEC: cbnz w16, [[SLOW]]
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp x16, 1
// SPEC: b.ne [[SLOW]]
// SPEC: ldrh w16, {{\[}}[[SEG]], 4]
// SPEC: cmp w16, 1
// SPEC: b.ne [[SLOW]]
//
// Under boxed doubles the young exit lands on the join, after the last
// declining check, where the value is encoded into a SmallHermesValue,
// mirroring HermesValue32::encodeHermesValue(): the ETag selects between the
// inline ("compressed HV64") case, the three pointer tags and Symbol. arm64
// dispatches with `cmn` against the negated ETag and tests
// inline-representability with one `tst` against a mask of low ones. A
// double whose low bits are not zero goes to the box arm; only a value with
// no SmallHermesValue encoding (RawHV32) still declines. Nothing at all is
// emitted for this in HV64, not even the comment.
// SPEC-HV32: {{^}}[[YOUNG]]:
// SPEC-HV32-NEXT: // Encode the value as a SmallHermesValue
// SPEC-HV32-NEXT: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-HV32-NEXT: cmn {{x[0-9]+}}, 0xA
// SPEC-HV32-NEXT: b.hi [[NOTNUM:L[0-9]+]]
// The compression test: with a 29-bit payload, the low 35 bits must be zero.
// SPEC-HV32: tst {{x[0-9]+}}, 0x7FFFFFFFF
// SPEC-HV32-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
// SPEC-HV32: lsr {{x[0-9]+}}, {{x[0-9]+}}, 0x20
// SPEC-HV32: {{^}}[[NOTNUM]]:
// SPEC-HV32: cmn {{x[0-9]+}}, 6
// SPEC-HV32: b.lo [[SYM:L[0-9]+]]
// A pointer: the HermesValue tag biased into the HV32 one, or-ed into the
// compressed pointer.
// SPEC-HV32: asr {{x[0-9]+}}, {{x[0-9]+}}, 1
// SPEC-HV32: add {{x[0-9]+}}, {{x[0-9]+}}, 4
// SPEC-HV32: sub {{x[0-9]+}}, {{x[0-9]+}}, x19
// SPEC-HV32: orr {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-HV32: {{^}}[[SYM]]:
// SPEC-HV32: cmn {{x[0-9]+}}, 9
// SPEC-HV32: b.ne [[SLOW]]
// The symbol tag goes in with an `add`, not an `orr`: the shift has just
// cleared the low three bits and 0b101 is not an AArch64 logical immediate.
// SPEC-HV32: mov {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-HV32: lsl {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-HV32: add {{x[0-9]+}}, {{x[0-9]+}}, 5
//
// The same dispatch in the boxed build without compressed pointers: a slot
// is a full eight bytes there, so the payload is 61 bits, the compression
// test is on the low three bits alone, the inline case needs no shift at
// all, and a pointer needs no compression either.
// SPEC-BOXED: {{^}}[[YOUNG]]:
// SPEC-BOXED-NEXT: // Encode the value as a SmallHermesValue
// SPEC-BOXED-NEXT: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x2F
// SPEC-BOXED-NEXT: cmn {{x[0-9]+}}, 0xA
// SPEC-BOXED-NEXT: b.hi [[NOTNUM:L[0-9]+]]
// SPEC-BOXED: tst {{x[0-9]+}}, 7
// SPEC-BOXED-NEXT: b.ne [[BOX:L[0-9]+]]{{$}}
// SPEC-BOXED: mov {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-BOXED: {{^}}[[NOTNUM]]:
// SPEC-BOXED: cmn {{x[0-9]+}}, 6
// SPEC-BOXED: b.lo [[SYM:L[0-9]+]]
// SPEC-BOXED: asr {{x[0-9]+}}, {{x[0-9]+}}, 1
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, 4
// SPEC-BOXED: orr {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
// SPEC-BOXED: {{^}}[[SYM]]:
// SPEC-BOXED: cmn {{x[0-9]+}}, 9
// SPEC-BOXED: b.ne [[SLOW]]
// SPEC-BOXED: mov {{w[0-9]+}}, {{w[0-9]+}}
// SPEC-BOXED: lsl {{x[0-9]+}}, {{x[0-9]+}}, 3
// SPEC-BOXED: add {{x[0-9]+}}, {{x[0-9]+}}, 5
//
// The store itself: one slot wide, which is four bytes only under compressed
// pointers. Under boxed doubles a young target, which reached it through the
// join, is then done: no barrier for any value.
// SPEC-HV64: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32: str [[VAL:w[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-HV32-NEXT: cmp [[SEG]], x16
// SPEC-HV32-NEXT: b.eq {{L[0-9]+$}}
// SPEC-BOXED: str [[VAL:x[0-9]+]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-BOXED-NEXT: cmp [[SEG]], x16
// SPEC-BOXED-NEXT: b.eq {{L[0-9]+$}}
//
// The card decision for an inline value, reached only for an old target
// holding a pointer to a young cell. The pointer test is made on the
// original HermesValue in every mode, which is why its tag compare is the
// same everywhere.
// SPEC: asr {{x[0-9]+}}, {{x[0-9]+}}, 0x30
// SPEC: cmn {{x[0-9]+}}, 3
// SPEC: b.lo [[DONE:L[0-9]+]]
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFFF
// SPEC: and {{x[0-9]+}}, {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC: ldr x16, [x19, {{[0-9]+}}]
// SPEC: cmp {{x[0-9]+}}, x16
// SPEC: b.ne [[DONE]]
//
// Under boxed doubles the inline path then branches to the card block it
// shares with the box arm, which follows at once (no counting block without
// -Xjit-emit-counters). The box arm: bump-allocate (a full young generation
// goes to the helper), write the double, tag the pointer, store it from the
// same register into the same slot, and recompute the segment start into
// the same register (it was an allocation temporary): a young target is
// done, an old one always gets its card.
// SPEC-HV32: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[BOX]]:
// SPEC-HV32-NEXT: // Box the double in the young generation
// SPEC-HV32-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
// SPEC-HV32: b.hi [[SLOW]]{{$}}
// SPEC-HV32: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
// SPEC-HV32-NEXT: sub [[CELL]], [[CELL]], x19
// SPEC-HV32-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
// SPEC-HV32-NEXT: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-HV32-NEXT: and [[SEG]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-HV32-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-HV32-NEXT: cmp [[SEG]], x16
// SPEC-HV32-NEXT: b.eq [[DONE]]{{$}}
// SPEC-HV32-NEXT: {{^}}[[DIRTY]]:
// SPEC-BOXED: {{^ *}}b [[DIRTY:L[0-9]+]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[BOX]]:
// SPEC-BOXED-NEXT: // Box the double in the young generation
// SPEC-BOXED-NEXT: ldr [[CELL:x[0-9]+]], [x19, {{[0-9]+}}]
// SPEC-BOXED: b.hi [[SLOW]]{{$}}
// SPEC-BOXED: str {{x[0-9]+}}, {{\[}}[[CELL]], {{[0-9]+}}]
// SPEC-BOXED-NEXT: orr [[CELL]], [[CELL]], {{(0x)?}}4
// SPEC-BOXED-NEXT: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC-BOXED-NEXT: and [[SEG]], {{x[0-9]+}}, 0xFFFFFFFFFFC00000
// SPEC-BOXED-NEXT: ldr x16, [x19, {{[0-9]+}}]
// SPEC-BOXED-NEXT: cmp [[SEG]], x16
// SPEC-BOXED-NEXT: b.eq [[DONE]]{{$}}
// SPEC-BOXED-NEXT: {{^}}[[DIRTY]]:
//
// The card-dirty store: the byte at segment start + (slot - segment start)
// >> 9.
// SPEC: sub {{x[0-9]+}}, {{x[0-9]+}}, [[SEG]]
// SPEC: lsr {{x[0-9]+}}, {{x[0-9]+}}, 9
// SPEC: mov w16, 1
// SPEC: strb w16, {{\[}}[[SEG]], {{x[0-9]+}}]
// In HV64 the card block jumps over the young-target store that follows it.
// SPEC-HV64: b [[DONE]]
// SPEC-HV64: [[YOUNG]]:
// SPEC-HV64: str [[VAL]], {{\[}}{{x[0-9]+}}]
// SPEC: [[DONE]]:
// And the unchanged helper call the guards fall back to.
// SPEC: [[SLOW]]:
// SPEC: call _jit_put_by_id
```

Spec resolution 1, arm64: `// SPEC: b [[DONE]]` and `// SPEC: [[YOUNG]]:`
become `SPEC-HV64`; every other directive active under HV64 is kept
verbatim and in order.

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='store-boxed-double|inline-emitted' \
    cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: `cmake-build-arm64` passes; on `-hv32` and `-boxed` the five
store-boxed-double files and the two arm64 pin files FAIL as they did on
x86-64 in Task 1 Step 10. Record each failure.

- [ ] **Step 3: The arm64 encoder**

In `lib/VM/JIT/arm64/JitEmitter-internal.h` replace the declaration block
from `#ifdef HERMESVM_BOXED_DOUBLES` (line 716) through `#endif` (line 745)
with:

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
/// Encode the HermesValue in \p xIn into the SmallHermesValue that a heap slot
/// holds, leaving the result in \p xOut. This is the inverse of
/// Emit_sh_shv_decode and mirrors HermesValue32::encodeHermesValue() case for
/// case, with one difference that is the whole reason it takes labels: a
/// double whose bits do not fit an inline "compressed HV64" has to be boxed
/// in a heap-allocated BoxedDouble, which this function does not allocate.
/// Such a value jumps to \p boxLab with nothing stored, where the caller
/// allocates the box. A value this code produces no encoding for -- a
/// RawHV32, or under HERMESVM_SANITIZE_HANDLES any number -- jumps to
/// \p slowLab, with nothing stored either.
///
/// Declared only under HERMESVM_BOXED_DOUBLES, unlike Emit_sh_shv_decode
/// which is always declared and emits nothing in the default build. The
/// decode can afford that because it works in place; an encode that produced
/// nothing would still owe its caller a register, so the caller selects
/// between this and using the HermesValue itself with the same #ifdef. Its
/// one caller is Emitter::emitSafeStoreOrSlow().
///
/// \param xOut receives the SmallHermesValue. Must differ from \p xIn.
/// \param xIn the HermesValue to encode; preserved.
/// \param xTemp scratch, clobbered. Must differ from \p xIn and \p xOut.
/// \param slowLab where to jump when the value has no encoding here.
/// \param boxLab where to jump with a double that needs a BoxedDouble.
///
/// NZCV is clobbered.
void emit_shv_encode_or_slow(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn,
    const a64::GpX &xTemp,
    const asmjit::Label &slowLab,
    const asmjit::Label &boxLab);
#endif
```

and delete `emit_shv_encode_for_slot_or_slow`, its doc comment and the
blank line after it (lines 747-778).

In `lib/VM/JIT/arm64/JitEmitter-internal.cpp` make the encoder edits of
Task 1 Step 11, in their arm64 form: the signature gains `const
asmjit::Label &boxLab` after `slowLab`; the dispatch comment's number line
becomes `//   number or compressible -> a right shift, or a BoxedDouble,
which the` / `//                             caller allocates: jump to
boxLab;`; the Handle-San comment's last sentence becomes the x86-64 one and
`(void)boxLab;` goes before `a.b(slowLab);`; and in the other arm

```cpp
  // A double that does not fit has to become a heap-allocated BoxedDouble.
  // Nothing has been stored at this point, so declining is free and the
  // helper does the allocation.
  a.b_ne(slowLab);
```

becomes

```cpp
  // A double that does not fit has to become a heap-allocated BoxedDouble,
  // which the caller allocates at boxLab. Nothing has been stored yet.
  a.b_ne(boxLab);
```

- [ ] **Step 4: The arm64 box arm**

In `lib/VM/JIT/arm64/JitEmitter-internal.cpp` add `#include
"hermes/VM/BoxedDouble.h"` after `#include "JitEmitter-internal.h"` (line
11). Replace the head of `emitSafeStoreOrSlow`, from `#if
HERMES_JIT_INLINE_SAFE_STORE` (line 487) through `  asmjit::Label doneLab
= a.newLabel();` (line 524), with:

```cpp
#if HERMES_JIT_INLINE_SAFE_STORE
#ifdef HERMESVM_BOXED_DOUBLES
void Emitter::emitSafeStoreOrSlow(
    const a64::GpX &loc,
    const a64::GpX &value,
    const a64::GpX &t1,
    const a64::GpX &t2,
    const a64::GpX &t3,
    JitCounter boxCounter,
    const asmjit::Label &slowLab) {
  assert(t1 != loc && t1 != value && "t1 must not alias loc or value");
  assert(t2 != loc && t2 != value && "t2 must not alias loc or value");
  assert(t3 != loc && t3 != value && "t3 must not alias loc or value");
  assert(t1 != t2 && t1 != t3 && t2 != t3 && "the temporaries must differ");
  // Every runtime word tested below is loaded into xScratch rather than into
  // a temporary -- x86-64 can compare against memory and so needs no
  // register at all for those, while arm64 must load first -- and consumed
  // by the next instruction: t1 holds the segment start from the first
  // instruction to the card store, and t2 and t3 belong to the encode and
  // the box arm. So nothing is held in xScratch across the box allocation,
  // whose ASan build calls out (see allocInYoung()).
#else
void Emitter::emitSafeStoreOrSlow(
    const a64::GpX &loc,
    const a64::GpX &shv,
    const a64::GpX &value,
    const a64::GpX &t1,
    const a64::GpX &t2,
    const asmjit::Label &slowLab) {
  assert(t1 != loc && t1 != value && "t1 must not alias loc or value");
  assert(t2 != loc && t2 != value && "t2 must not alias loc or value");
  assert(t1 != t2 && "the two temporaries must differ");
  assert(
      shv != loc && shv != t1 && "the encoded value must survive to the store");
  // In the default heap-value mode a slot holds the HermesValue itself, so
  // the tiers pass \p value as \p shv. Every runtime word tested below is
  // loaded into xScratch rather than into a temporary: x86-64 can compare
  // against memory and so needs no register at all for those, while arm64
  // must load first, and t1 holds the segment start the card store indexes
  // off.
#endif

  // The two 64-bit runtime words below are loaded with LDR's unsigned-offset
  // form, whose immediate is a 12-bit multiple of the access width. Both
  // offsets are well inside that today (2136 and 5416), but they are
  // offsetof()s into Runtime, which grows: pin them so that outgrowing the
  // encoding breaks the build rather than making every JIT compilation fail
  // at emission time.
  static_assert(
      RuntimeOffsets::runtimeHadesYGStart % 8 == 0 &&
          RuntimeOffsets::runtimeHadesYGStart < kMaxInlineBaseOffset,
      "the young-gen start must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::runtimeHadesCompacteeStart % 8 == 0 &&
          RuntimeOffsets::runtimeHadesCompacteeStart < kMaxInlineBaseOffset,
      "the compactee start must be reachable by an unsigned-offset LDR");

  comment("// Inline store with barrier predicate");

#ifdef HERMESVM_BOXED_DOUBLES
  // A young target skips the barrier checks but not the encode: it jumps to
  // the join, where every path that stores begins.
  asmjit::Label joinLab = a.newLabel();
  const asmjit::Label &youngTargetLab = joinLab;
  asmjit::Label boxLab = a.newLabel();
  asmjit::Label dirtyCardLab = a.newLabel();
  // With -Xjit-emit-counters a full young generation goes through an
  // out-of-line block that counts it; otherwise straight to the helper.
  asmjit::Label slowFullLab = emitCounters_ ? a.newLabel() : slowLab;
#else
  asmjit::Label youngTargetLab = a.newLabel();
#endif
  asmjit::Label doneLab = a.newLabel();
```

Keep lines 526-581 (from `  // The mask that turns a pointer into the
start of its segment.` through the segment-size test's `
a.b_ne(slowLab);`) unchanged. Replace the rest, from `  // The barrier is
now known to be at most a card-dirty. Store. The slot is` (line 583)
through `#endif // HERMES_JIT_INLINE_SAFE_STORE` (line 631), with:

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  // The join. Live: loc, value and t1, the segment start; t2 and t3 are
  // free, and nothing is live in a vector register or xScratch, which is
  // what allocInYoung() below needs under ASan. The structural guards and
  // the barrier checks have run; the declines left are the encode's RawHV32
  // and Handle-San rejections, inside it, and the box arm's full young
  // generation, which branches from the bump before the new level is
  // stored. Each precedes the slot store and the publication of a box. The
  // slot is one SmallHermesValue wide -- four bytes under compressed
  // pointers, eight otherwise -- which is why the stores go through
  // emit_store_shv() rather than a bare str.
  a.bind(joinLab);
  emit_shv_encode_or_slow(a, t2, value, t3, slowLab, boxLab);
  emit_store_shv(a, t2, a64::Mem(loc));
  // HadesGC::writeBarrier() step (1) again: a young target needs no barrier
  // for any value.
  a.ldr(xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.cmp(t1, xScratch);
  a.b_eq(doneLab);
#else
  // The barrier is now known to be at most a card-dirty. Store. The slot is
  // one SmallHermesValue wide -- four bytes under compressed pointers, eight
  // otherwise -- which is why this goes through emit_store_shv() rather than
  // a bare str.
  emit_store_shv(a, shv, a64::Mem(loc));
#endif

  // relocationWriteBarrier() is only reached for a pointer value. The test is
  // made on the ORIGINAL HermesValue rather than on what was just stored,
  // which is both cheaper and more accurate: a HermesValue carries its
  // pointer uncompressed, which is what the segment compare below needs. The
  // one pointer a SmallHermesValue has that a HermesValue does not -- a
  // BoxedDouble -- never reaches this test: the box arm below stores it. \p
  // t2 is dead once the store above has consumed it.
  emit_sh_ljs_get_tag(a, t2, value);
  emit_sh_ljs_tag_is_pointer(a, t2);
  a.b_lo(doneLab);

  // t2 = the start of the segment containing the pointed-to cell. Only an
  // old-to-young pointer needs a card; old-to-old does not, and the compactee
  // cases were declined above. The young-gen start comes back into xScratch,
  // the same non-allocated scratch the tests above used.
  emit_sh_ljs_get_pointer(a, t2, value);
  a.and_(t2, t2, kSegmentMask);
  a.ldr(xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.cmp(t2, xScratch);
  a.b_ne(doneLab);

#ifdef HERMESVM_BOXED_DOUBLES
  a.b(dirtyCardLab);

  if (emitCounters_) {
    // Out of line: doneLab falls through into the tier's branch to its
    // continuation, so nothing may sit between them.
    a.bind(slowFullLab);
    comment("// Young generation full: count the fallback");
    emitIncrementCounter(JitCounter::NumStoreBoxFallbacks);
    a.b(slowLab);
  }

  // The box arm, HermesValue32::encodeHermesValue()'s BoxedDouble::create()
  // inline: bump-allocate the box in the young generation (t2 the cell, t1
  // and t3 its temporaries), write the double into it, and store the tagged
  // compressed pointer. A full young generation branches before the new
  // level is published, so a box is never allocated and abandoned. Nothing
  // here is a GC safepoint: in ASan builds the bump calls
  // __asan_unpoison_memory_region with x0-x15 saved, and that is the only
  // call.
  a.bind(boxLab);
  comment("// Box the double in the young generation");
  allocInYoung(
      CellKind::BoxedDoubleKind,
      cellSize<BoxedDouble>(),
      t2,
      t1,
      t3,
      slowFullLab);
  static_assert(
      RuntimeOffsets::boxedDoubleValue % 8 == 0 &&
          RuntimeOffsets::boxedDoubleValue < maxNaturalBaseOffset(8),
      "the double in a BoxedDouble must be reachable by an unsigned-offset "
      "STR");
  a.str(value, a64::Mem(t2, RuntimeOffsets::boxedDoubleValue));
  // encodePointerImpl(box, Tag::BoxedDouble): the compressed pointer with the
  // tag in its low bits, which heap alignment leaves clear. 0b100 is a single
  // bit, an AArch64 logical immediate.
  emit_sh_cp_encode_non_null(a, t2);
  static_assert(
      (uint32_t)HermesValue32::Tag::BoxedDouble <
          ((uint32_t)1 << RuntimeOffsets::kShvTagBits),
      "the BoxedDouble tag must fit the bits heap alignment leaves clear");
  assert(
      a64::Utils::isLogicalImm((uint64_t)HermesValue32::Tag::BoxedDouble, 64) &&
      "the BoxedDouble tag must encode as a logical immediate");
  a.orr(t2, t2, (uint64_t)HermesValue32::Tag::BoxedDouble);
  emit_store_shv(a, t2, a64::Mem(loc));
  emitIncrementCounter(boxCounter);
  // t1 was an allocation temporary: recompute the segment start. A young
  // target is done; an old one always needs its card, because the box is
  // young by construction.
  a.and_(t1, loc, kSegmentMask);
  a.ldr(xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.cmp(t1, xScratch);
  a.b_eq(doneLab);

  a.bind(dirtyCardLab);
#endif

  // FixedSizeHeapSegment::dirtyCardForAddress(loc), inline:
  //   segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  // The card array base is the segment start itself, since
  // RuntimeOffsets::segmentInlineCards is 0, so the store needs no
  // displacement on top of the base+index addressing. The runtime writes this
  // byte with a relaxed atomic store, which on arm64 is this same
  // instruction.
  static_assert(
      RuntimeOffsets::segmentInlineCards == 0,
      "the card store indexes off the segment start with no displacement");
  a.sub(t2, loc, t1);
  a.lsr(t2, t2, RuntimeOffsets::kLogCardSize);
  a.mov(xScratch.w(), RuntimeOffsets::kCardDirty);
  a.strb(xScratch.w(), a64::Mem(t1, t2));
#ifndef HERMESVM_BOXED_DOUBLES
  a.b(doneLab);

  a.bind(youngTargetLab);
  emit_store_shv(a, shv, a64::Mem(loc));
#endif

  a.bind(doneLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE
```

(The existing `// The mask that turns a pointer ...` block defines
`kSegmentMask` as a `uint64_t` run of ones and asserts it is a logical
immediate, arm64/JitEmitter-internal.cpp:526-534; the box arm's `and_`
reuses it.)

- [ ] **Step 5: The arm64 declarations and docs**

In `lib/VM/JIT/arm64/JitEmitter.h` replace lines 1560-1602 (from `  ///
Emit an inline store of the already-encoded slot value in \p shv to the`
through `  /// requirement holds by construction rather than by
convention.`) with the same text as Task 1 Step 13's first block; keep
lines 1603-1629 (PRECONDITION, asymmetry, segment-size); and replace lines
1630-1655 (from the `  ///` before `  /// \param loc address of the slot;
preserved.` through `      const asmjit::Label &slowLab);`) with:

```cpp
  ///
  /// \param loc address of the slot; preserved.
  /// \param shv (default heap-value mode only) the value to store, which is
  ///   \p value: there a slot holds the HermesValue itself.
  /// \param value the original 64-bit HermesValue; preserved.
  /// \param t1 scratch, clobbered. Must differ from \p loc and \p value.
  /// \param t2 scratch, clobbered. Must differ from \p loc and \p value.
  /// \param t3 (boxed doubles only) scratch, clobbered: the encode's
  ///   temporary and the box allocation's second one. Must differ from
  ///   \p loc, \p value, \p t1 and \p t2.
  /// \param boxCounter (boxed doubles only) the counter the box arm
  ///   increments after its store under -Xjit-emit-counters.
  /// \param slowLab where to jump when the store was NOT performed.
  ///
  /// NZCV is clobbered, and so is xScratch: unlike x86-64, which compares
  /// against memory directly, arm64 has to load each of the runtime words it
  /// tests into a register, and no temporary is free to be it -- \p t1 holds
  /// the segment start the card store indexes off, and \p t2 and \p t3 belong
  /// to the encode and the box arm. In the default heap-value mode nothing
  /// else is touched and no call is emitted, so no register needs to be
  /// synced or freed around it. Under boxed doubles the same holds except in
  /// ASan builds, where the box allocation calls
  /// __asan_unpoison_memory_region with x0-x15 saved around it
  /// (bumpAllocAndUnpoison()): no live value may be held in a vector temp,
  /// xScratch or xScratch2 across this function.
#ifdef HERMESVM_BOXED_DOUBLES
  void emitSafeStoreOrSlow(
      const a64::GpX &loc,
      const a64::GpX &value,
      const a64::GpX &t1,
      const a64::GpX &t2,
      const a64::GpX &t3,
      JitCounter boxCounter,
      const asmjit::Label &slowLab);
#else
  void emitSafeStoreOrSlow(
      const a64::GpX &loc,
      const a64::GpX &shv,
      const a64::GpX &value,
      const a64::GpX &t1,
      const a64::GpX &t2,
      const asmjit::Label &slowLab);
#endif
```

Document `allocInYoung`'s ASan precondition on the arm64 declarations
(spec "Comments and docs"): in the doc comments of `bumpAllocAndUnpoison`
(before `void bumpAllocAndUnpoison(`, line 2096) and `allocInYoung`
(before `void allocInYoung(`, line 2122), after the `\param slowPathLab`
line, add

```cpp
  ///
  /// Precondition (ASan builds only): no live value may be held in any
  /// vector temp, nor in xScratch (x16) or xScratch2 (x17), across this
  /// call -- the ASan save loop pushes only the 16 GP temps (x0-x15), and the
  /// call to __asan_unpoison_memory_region may clobber every other
  /// caller-saved register. \p xOut, \p xTemp1, and \p xTemp2 must not be
  /// xScratch or xScratch2 either.
```

(On `allocInYoung` the paragraph reads instead "Precondition (ASan builds only): same as
bumpAllocAndUnpoison() -- no live value in any vector temp, xScratch or
xScratch2 across the call, and \p xOut, \p xTemp1, \p xTemp2 must not be
either scratch register.", as the x86-64 declaration does,
x86-64/JitEmitter.h:2207-2210.)

- [ ] **Step 6: The arm64 tiers**

In `lib/VM/JIT/arm64/JitEmitter-property.cpp`, `emitPutByValFastArrayTier`:
after `  HWReg hwTemp2 = allocTempGpX();` (line 61) add

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  // emitSafeStoreOrSlow()'s third temporary, for the encode and the box arm.
  // Allocated last, so the registers above are the default mode's.
  HWReg hwTemp3 = allocTempGpX();
#endif
```

after `  const a64::GpX xTemp2 = hwTemp2.a64GpX();` add

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  const a64::GpX xTemp3 = hwTemp3.a64GpX();
#endif
```

after `  freeReg(hwTemp2);` add

```cpp
#ifdef HERMESVM_BOXED_DOUBLES
  freeReg(hwTemp3);
#endif
```

delete the encode block (lines 84-96: the comment `  // The value has to
be encoded into the SmallHermesValue an element slot` through `
emit_shv_encode_for_slot_or_slow(a, xTemp2, xValue, xTemp1, helperLab);`
and the blank line after it); in the flags comment (lines 125-127) replace
`It goes in xScratch, not in a\n  // temporary: xTemp2 is carrying the
encoded value across every guard in\n  // this tier.` with `It goes in
xScratch, the backend's non-allocated\n  // scratch, so this sequence
needs no temporary of its own.` (re-wrapped); and replace the final call
(lines 248-249) with

```cpp
  // Store, if the write barrier for it is a no-op or a card-dirty. Under
  // boxed doubles this also encodes the value, boxing a double that needs
  // it.
#ifdef HERMESVM_BOXED_DOUBLES
  emitSafeStoreOrSlow(
      xLoc,
      xValue,
      xTemp1,
      xTemp2,
      xTemp3,
      JitCounter::NumPutByValBoxes,
      helperLab);
#else
  emitSafeStoreOrSlow(xLoc, xValue, xValue, xTemp1, xTemp2, helperLab);
#endif
```

`emitPutByIdInlineTier`: the same three insertions after `  HWReg hwTemp2
= allocTempGpX();` (line 2530), `  const a64::GpX xTemp2 =
hwTemp2.a64GpX();` (line 2535) and `  freeReg(hwTemp2);` (line 2538);
delete the encode block (lines 2544-2554); replace the comment `  //
xScratch, not xTemp2: xTemp2 is carrying the encoded value to the store.`
(line 2566) with `  // xScratch holds the id when it is not a compare
immediate.`; and replace the final call (lines 2590-2591) with the
PutByVal form above, using `JitCounter::NumPutByIdBoxes`.

In the GetByVal tier comment (lines 1716-1722) make the replacement of
Task 1 Step 14 (`emit_shv_encode_for_slot_or_slow()` ->
`emit_shv_encode_or_slow()`, "to decline:" -> "to decline or to box:").
`grep -rn 'emit_shv_encode_for_slot_or_slow' lib include` must now print
nothing.

- [ ] **Step 7: Build all three arm64 trees and run**

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-arm64 cmake-build-arm64-hv32 cmake-build-arm64-boxed; do
  cmake --build $t --target hermes &&
  ./aarch64/qemu-sanity.sh $t &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: no `FAIL`; the five store-boxed-double tests pass on every arm64
tree with the x86-64 counts. Pin regex fixes follow Task 1 Step 15's rule.
An arm64-only failure is a finding about the port: debug it
(superpowers:systematic-debugging); never gate the test.

- [ ] **Step 8: HV64 emitted code unchanged, and the GC conditions**

```bash
$S/hv32impl/hv64-ident.sh $S/hv32impl/base/hermes-arm64 \
  cmake-build-arm64/bin/hermes $S/hv32impl/ident-task2-arm64 --qemu |
  tee $S/hv32impl/ident-task2-arm64.log
```

Expected: six `IDENTICAL` with 0 differences (helper addresses may move;
the arm64 image base, printed first, may differ between the two binaries).
Unresolved heap-address differences only: stop and report (Task 1 Step 1).
The arm64 stale-address control, on the real base/head pair:

```bash
L=$S/hv32impl/ident-task2-arm64
AB0=$(awk '/^image base:/ {print $4}' $L.log | tr -d ,)
AB1=$(awk '/^image base:/ {print $6}' $L.log)
python3 -I $S/hv32impl/hv64-sens.py $L/putbyval-arm64.before.txt \
  $S/hv32impl/base/hermes-arm64 $AB0 aarch64-linux-gnu-nm $L \
  cmake-build-arm64/bin/hermes $AB1
```

Expected: all four alterations judged as stated, exit 0. Exit 2
with `no helper whose address differs` means no helper of that dump
moved between the two binaries, so no stale address can arise there; say
so in the report.
Then run Task 1 Step 17's loop over the three arm64 trees, with
`H="qemu-aarch64-static -L /usr/aarch64-linux-gnu $t/bin/hermes
-fno-inline -Xjit-crash-on-error -Xhermes-internal-test-methods
-gc-print-collection-stats"`. Expected: every `gc-check.py` run exits 0.

- [ ] **Step 9: Prove the arm64 box arm is pinned**

On `cmake-build-arm64-hv32` (rebuild host first; rebuild after each
change and restore; `LIT_FILTER='store-boxed-double|inline-emitted'`), in
`lib/VM/JIT/arm64/JitEmitter-internal.cpp`:
- Mutation A: `emit_shv_encode_or_slow(a, t2, value, t3, slowLab,
  boxLab);` -> `... slowLab, slowLab);`. Expected FAIL: byval, byid, card,
  ygfull, both arm64 pin files.
- Mutation B: the box arm's `a.b_eq(doneLab);` after the recomputed young
  compare -> `a.b(doneLab);`. Expected FAIL: the three card RUN lines
  (`verifyCardTable()`), the pin files. Repeat on
  `cmake-build-arm64-boxed`.
- Mutation D: `a.str(value, a64::Mem(t2,
  RuntimeOffsets::boxedDoubleValue));` -> `a.str(value.w(), a64::Mem(t2,
  RuntimeOffsets::boxedDoubleValue));`. Expected FAIL: byval, byid, values.

- [ ] **Step 10: x86-64 still green**

```bash
for t in cmake-build-x86jit cmake-build-x86jit-hv32 cmake-build-x86jit-boxed; do
  cmake --build $t --target hermes &&
  LIT_FILTER='jit/' cmake --build $t --target check-hermes || echo "FAIL $t"
done
```

Expected: pass (only the gate lines of the shared tests changed).

- [ ] **Step 11: Commit**

```bash
git clang-format HEAD -- lib/VM/JIT/arm64/JitEmitter-internal.h \
  lib/VM/JIT/arm64/JitEmitter-internal.cpp lib/VM/JIT/arm64/JitEmitter.h \
  lib/VM/JIT/arm64/JitEmitter-property.cpp
git add lib/VM/JIT/arm64/JitEmitter-internal.h \
  lib/VM/JIT/arm64/JitEmitter-internal.cpp lib/VM/JIT/arm64/JitEmitter.h \
  lib/VM/JIT/arm64/JitEmitter-property.cpp \
  test/jit/putbyval-inline-emitted-arm64.js \
  test/jit/putbyid-inline-emitted-arm64.js \
  test/jit/store-boxed-double-byval.js test/jit/store-boxed-double-byid.js \
  test/jit/store-boxed-double-card.js test/jit/store-boxed-double-ygfull.js \
  test/jit/store-boxed-double-values.js
git commit   # "JIT: box doubles inline in the arm64 store tiers"
```

---

### Task 3: doc/JIT.md and test comments

**Files:**
- Modify: `doc/JIT.md:462-475` (Inline allocation), `:528-556` (decision
  sequence), `:628-669` (SmallHermesValue, encode, temp2, card
  paragraphs), `:680-694` (Tests), `:723-731` (Box2D), `:1431-1437`
  (counters)
- Modify: `test/jit/inline-store-shv-shapes.js:22-40, 143-145` (comments
  only)

**Interfaces:**
- Consumes: Tasks 1-2 as committed (names, comments, counters, tests).

- [ ] **Step 1: "Inline allocation"**

In the paragraph at doc/JIT.md:464-475, after the sentence ending
`` `alloc2InYoung` allocates object+propstorage pairs. `` add: "Under
boxed doubles the two inline store tiers allocate too: the box arm of
`emitSafeStoreOrSlow` bump-allocates a `BoxedDouble` with `allocInYoung`
(see "Inline write barrier"); in ASan builds that puts the unpoison call
inside the predicate, with the GP temps saved."

- [ ] **Step 2: The decision sequence**

Replace doc/JIT.md:528-531 (`The design -- the decision sequence, the
first-unit precondition, the callers' bounds, the encode's per-tag rules
and its decline case -- is architecture-independent;`) with "The design --
the decision sequence, the first-unit precondition, the callers' bounds,
the encode's per-tag rules and the box arm -- is architecture-independent;"
(re-wrapped, rest of the paragraph unchanged), and replace the sequence
block and its "That is" paragraph (lines 536-556) with:

```
**The decision sequence**, given `loc` (slot address) and `value` (the
64-bit HermesValue) in registers:

    segLoc = loc & ~(kSegmentUnitSize - 1)
    if (segLoc == runtime.heap_.youngGen_.lowLim_)      → STORE
    if (runtime.heap_.ogMarkingBarriers_ != 0)          → HELPER
    if (compactee active)                               → HELPER
    if (word at segLoc+4 (shiftedSegmentSize) != 1)      → HELPER
  STORE:
    shv = encode(value)                                  (boxed doubles only;
                                                          needs a box → BOX)
    store shv
    if (segLoc is young)                                 → done
    if (value is a young pointer)
      card byte at segLoc + ((loc - segLoc) >> 9) = 1   (dirty)
    done
  BOX:                                                   (boxed doubles only)
    cell = bump-allocate a BoxedDouble (young)           (full → HELPER)
    cell->value_ = value
    store compress(cell) | Tag::BoxedDouble
    if (segLoc is old)
      card byte at segLoc + ((loc - segLoc) >> 9) = 1   (dirty)

That is: a young-generation target needs no barrier at all; a marking
collection in progress or a compaction in progress both need the full C++
barrier; and a segment whose card array is not the inline one (see below)
cannot be dirtied by this code. Otherwise the store happens, and the card
is dirtied exactly when the stored value is a young pointer -- which a box
always is. Only the structural guards and the barrier checks precede the
encode; the RawHV32 rejection is inside the encode, and the
young-generation-full fallback branches from the bump. Every decline
precedes the slot store and the publication of a successful allocation,
so nothing is stored, and no box is left allocated, before a decline.
```

- [ ] **Step 3: The SmallHermesValue paragraphs**

Replace doc/JIT.md:628-669 (from `**A slot holds a SmallHermesValue, not a
HermesValue,**` through `pointer uncompressed, which is what the
young-segment compare needs.`) with:

```
**A slot holds a SmallHermesValue, not a HermesValue,** and the two differ
in every mode that defines `HERMESVM_BOXED_DOUBLES` (both `HEAP_HV_PREFER32`
and `HEAP_HV_BOXED`). `emit_shv_encode_or_slow` (`JitEmitter-internal.cpp`)
mirrors `HermesValue32::encodeHermesValue()` case for case -- an ETag
dispatch over the inline ("compressed HV64") values, the three pointer tags
and Symbol -- with one difference, which is why it takes two labels. A
double whose low bits are not zero has no inline encoding: it has to be
boxed in a heap-allocated `BoxedDouble`. The encoder jumps to `boxLab`, and
the box arm of `emitSafeStoreOrSlow` allocates the box in the young
generation (`allocInYoung`), writes the double, and stores the tagged
compressed pointer. Only a full young generation declines it, to the
helper; under `-Xjit-emit-counters` through an out-of-line block that
counts `NumStoreBoxFallbacks`. A RawHV32, and under
`HERMESVM_SANITIZE_HANDLES` every number, still declines to the helper.

**The encode runs inside the predicate, after its declining checks**
(doc/superpowers/specs/2026-10-29-jit-inline-boxed-double-store-design.md).
It used to run first, ahead of the tiers' guards, when most declines were
box declines: a scratch counter over the HV32 Release build put the decline
rate at 63.6% on Octane Box2D, and rejecting those values before the guards
halved Box2D's regression. Once a box no longer declines, the early encode
has nothing cheap left to reject, and every decline must precede the slot
store and the publication of a successful box allocation, so that a box is
never allocated and abandoned and nothing is stored before a decline: the
structural and barrier checks precede the encode, the encode's RawHV32
rejection precedes the box arm, and a full young generation branches from
the bump before the new level is stored. The cost: a box store declined for
marking or compaction now runs the tier's guards first, and every store
under boxed doubles repeats the young-target compare after storing.

Under boxed doubles the predicate takes a third temporary, `t3`, which
each tier picks in its prologue beside the others (no code is emitted for
that; the x86-64 PutByVal tier then uses at most 7 of its 8 GP temps). At
the join `loc`, `value` and `t1` (the segment start) are live; the encode
writes the SmallHermesValue to `t2` with `t3` as scratch; the box arm uses
`t2` for the cell and `t1` and `t3` as allocation temporaries, then
recomputes `t1` from `loc`. Nothing is live in a vector register or in
`xScratch` there, which is what `allocInYoung` requires in ASan builds,
where it calls `__asan_unpoison_memory_region` with the GP temps saved. On
arm64 every mask, wide immediate and runtime word the guards and the
predicate test goes to `xScratch` (x16), the backend's non-allocated
scratch, and is consumed by the next instruction.

The card decision for an inline value is phrased in terms of the original
64-bit value: the two agree on which values are pointers, and a HermesValue
carries its pointer uncompressed, which is what the young-segment compare
needs. A box is young by construction, so its store into an old slot
dirties the card unconditionally.
```

- [ ] **Step 4: "Tests"**

In the Tests paragraph (doc/JIT.md:680-694), replace `the last of those
drives every SmallHermesValue shape through both tiers, including the
non-compressible doubles that must take the helper.` with `the last of
those drives every SmallHermesValue shape through both tiers, including
the non-compressible doubles the box arm stores.`, and append after the
sentence naming the four emitted-code files: "`test/jit/store-boxed-double-
byval.js` and `-byid.js` count the inline boxes per tier
(`NumPutByValBoxes`, `NumPutByIdBoxes`) into young and old targets;
`-card.js` stores one box into an old array element, an old direct slot
and an old out-of-line slot and forces a young collection, so a missing
card fails in `verifyCardTable()`; `-ygfull.js` fills the young generation
with boxes, with and without counters (`NumStoreBoxFallbacks`); and
`-values.js` round-trips the encoding's edge values through both tiers and
a duo PutByVal site against the interpreter."

- [ ] **Step 5: The Box2D paragraph and the counters**

Replace doc/JIT.md:723-731 (`Box2D is the one benchmark the tiers lose
on,` through `series.`) with:

```
Box2D was the one benchmark the tiers lost on, because two thirds of its
stores declined as box declines; with the box arm those stores complete
inline. What remains on the helper path under boxed doubles is a store
declined for marking or compaction, a full young generation, and a failed
guard. The measurements before and after are in the "Delivered" section of
doc/superpowers/specs/2026-10-29-jit-inline-boxed-double-store-design.md.
```

In the counters bullet (doc/JIT.md:1431-1437) add `NumPutByValBoxes,
NumPutByIdBoxes, NumStoreBoxFallbacks` after `NumByIdStringHits` in the
list, and to "Emitted code increments NumCall, NumCallSlow,
NumByIdChainHits and NumByIdStringHits inline" add "and the three box
counters".

- [ ] **Step 6: inline-store-shv-shapes.js comments**

In `test/jit/inline-store-shv-shapes.js` (comments only; no CHECK line
changes):
- replace lines 22-27 (`// ENCODE first (Emitter::emitSafeStoreOrSlow ->
  emit_shv_encode_or_slow).` through `// runtime helper without storing
  anything. Every arm of that dispatch is`) with
  ```
  // ENCODE first (Emitter::emitSafeStoreOrSlow, through
  // emit_shv_encode_or_slow). The encoding is per-tag, and one case
  // allocates: a double whose bits do not survive the compression is boxed
  // in a BoxedDouble, which the tiers bump-allocate in the young generation
  // (only a full young generation sends it to the runtime helper, which
  // allocates it there). Every arm of that dispatch is
  ```
- replace `//   0.1, 1e300                        doubles that do NOT: helper +
  BoxedDouble` with `//   0.1, 1e300                        doubles that
  do NOT: an inline BoxedDouble`;
- replace lines 143-145 (`// A non-compressible double stored repeatedly
  into an old object: every one` through `// double must survive the
  collections that follow.`) with
  ```
  // A non-compressible double stored repeatedly into an old object: the
  // tier boxes each one in the young generation and dirties the slot's
  // card, and the box must survive the collections that follow -- a missed
  // card would leave the old slot pointing at a box a young collection
  // freed.
  ```

- [ ] **Step 7: Check and commit**

```bash
LIT_FILTER='inline-store-shv' cmake --build cmake-build-x86jit-hv32 --target check-hermes
git add doc/JIT.md test/jit/inline-store-shv-shapes.js
git commit   # "JIT: document inline boxed-double stores"
```

---

### Task 4: Verification, benchmarks, Delivered, dz

**Files:**
- Modify: `doc/superpowers/specs/2026-10-29-jit-inline-boxed-double-store-design.md`
  (append "Delivered")
- Modify: `dz/issues/01a185d0-5d43-700c-a5b9-5868c8a9de62.md`,
  `dz/issues/01a04e00-0811-70c9-809b-0822bf587426.md` (through `dz`)
- Create: one dz issue (through `dz add`)

**Interfaces:**
- Consumes: BASE and the base binaries (Task 1 Step 1), every task's
  report, `$S/hv32diag/instrumentation.diff` (the dz 01a185d0-5d43
  instrumentation, from the root-cause session).

- [ ] **Step 1: HV64 emitted code, final**

```bash
cmake --build cmake-build-host --target hermesc shermes
for t in cmake-build-x86jit cmake-build-arm64; do cmake --build $t --target hermes; done
$S/hv32impl/hv64-ident.sh $S/hv32impl/base/hermes-x86jit \
  cmake-build-x86jit/bin/hermes $S/hv32impl/ident-final-x86 --corpus |
  tee $S/hv32impl/ident-final-x86.log
$S/hv32impl/hv64-ident.sh $S/hv32impl/base/hermes-arm64 \
  cmake-build-arm64/bin/hermes $S/hv32impl/ident-final-arm64 --qemu |
  tee $S/hv32impl/ident-final-arm64.log
XB=$(awk '/^image base:/ {print $6}' $S/hv32impl/ident-final-x86.log)
AB=$(awk '/^image base:/ {print $6}' $S/hv32impl/ident-final-arm64.log)
XB0=$(awk '/^image base:/ {print $4}' $S/hv32impl/ident-final-x86.log | tr -d ,)
AB0=$(awk '/^image base:/ {print $4}' $S/hv32impl/ident-final-arm64.log | tr -d ,)
python3 -I $S/hv32impl/hv64-sens.py \
  $S/hv32impl/ident-final-x86/putbyid-x86.before.txt \
  $S/hv32impl/base/hermes-x86jit $XB0 nm $S/hv32impl/ident-final-x86 \
  cmake-build-x86jit/bin/hermes $XB
python3 -I $S/hv32impl/hv64-sens.py \
  $S/hv32impl/ident-final-arm64/putbyval-arm64.before.txt \
  $S/hv32impl/base/hermes-arm64 $AB0 aarch64-linux-gnu-nm \
  $S/hv32impl/ident-final-arm64 cmake-build-arm64/bin/hermes $AB
```

Expected: every line `IDENTICAL` with 0 differences (x86-64: the six
inputs and every JIT test; arm64: the six inputs), and both sensitivity
runs judge all four alterations as stated, the stale case now on the real
base/head pair (exit 2 `no helper whose address differs`: report that no
helper of the dump moved). Spec criterion 2. (`$4` and `$6` are the
`before` and `after` bases of the `image base:` line.)

- [ ] **Step 2: Release trees and bytecode**

```bash
for t in cmake-build-jit2 cmake-build-jit2-hv32; do
  cmake --build $t --target hermes hermesc
done
```

Write `$S/hv32impl/bench.sh`:

```bash
#!/bin/bash
# Octane without RegExp: base/head x HV64/HV32 JIT, plus head HV32 interp.
# 5 reps; per rep and benchmark the five configurations run in an order
# rotated by the rep. Release, HERMESVM_ALLOW_JIT=2, taskset -c 2, from
# bytecode compiled by each build's own hermesc -O.
S=/tmp/claude-1000/-home-tmikov-work-hermes-x86-jit/d7daed21-e94e-4375-aaac-240004bf2ed9/scratchpad
P=$S/hv32impl/bench
R=/home/tmikov/work/hermes-x86-jit
B="richards deltablue crypto raytrace earley-boyer splay navier-stokes pdfjs mandreel gbemu box2d zlib typescript"
declare -A BIN=(
  [base64]=$S/hv32impl/base/cmake-build-jit2
  [head64]=$R/cmake-build-jit2/bin
  [base32]=$S/hv32impl/base/cmake-build-jit2-hv32
  [head32]=$R/cmake-build-jit2-hv32/bin
  [interp32]=$R/cmake-build-jit2-hv32/bin
)
declare -A FLAG=([base64]=-Xjit [head64]=-Xjit [base32]=-Xjit
                 [head32]=-Xjit [interp32]=-Xjit=off)
declare -A HBC=([base64]=base64 [head64]=head64 [base32]=base32
                [head32]=head32 [interp32]=head32)
CONFIGS=(base64 head64 base32 head32 interp32)
mkdir -p $P/logs
for c in base64 head64 base32 head32; do
  mkdir -p $P/hbc/$c
  for b in $B; do
    ${BIN[$c]}/hermesc -O -emit-binary -out $P/hbc/$c/$b.hbc \
      $R/benchmarks/octane/$b.js || exit 1
  done
done
echo "rep,benchmark,config,score,exit" > $P/scores.csv
for rep in 0 1 2 3 4; do
  for b in $B; do
    for k in 0 1 2 3 4; do
      c=${CONFIGS[$(( (k + rep) % 5 ))]}
      out=$(taskset -c 2 ${BIN[$c]}/hermes ${FLAG[$c]} \
        $P/hbc/${HBC[$c]}/$b.hbc 2>&1); ec=$?
      echo "$out" > $P/logs/$rep.$b.$c.log
      score=$(echo "$out" | grep -E '^[A-Za-z0-9-]+:? [0-9.]+$' | tail -1 |
        awk '{print $NF}')
      echo "$rep,$b,$c,$score,$ec" >> $P/scores.csv
    done
  done
done
echo DONE > $P/done
```

and `$S/hv32impl/summ.py`:

```python
import csv, math, statistics as st, sys
P = sys.argv[1]
B = ("richards deltablue crypto raytrace earley-boyer splay navier-stokes "
     "pdfjs mandreel gbemu box2d zlib typescript").split()
d, bad = {}, []
for r in csv.DictReader(open(P + "/scores.csv")):
    if r["exit"] != "0" or not r["score"]:
        bad.append(r)
        continue
    d.setdefault((r["benchmark"], r["config"]), []).append(float(r["score"]))
g = lambda v: math.exp(sum(map(math.log, v)) / len(v))
for mode in ("64", "32"):
    print(f"\nHV{mode} JIT, base vs head")
    print("| benchmark | base median [min-max] | head median [min-max] "
          "| head/base | verdict |")
    print("|---|---|---|---|---|")
    rs = []
    for b in B:
        o, n = d[(b, "base" + mode)], d[(b, "head" + mode)]
        mo, mn = st.median(o), st.median(n)
        rs.append(mn / mo)
        overlap = not (min(n) > max(o) or max(n) < min(o))
        v = "noise" if overlap else ("faster" if mn > mo else "slower")
        print(f"| {b} | {mo:g} [{min(o):g}-{max(o):g}] "
              f"| {mn:g} [{min(n):g}-{max(n):g}] | {mn / mo:.3f} | {v} |")
    print(f"| geomean | | | {g(rs):.3f} | |")
print("\nHV32 JIT/interp")
print("| benchmark | interp | base JIT | x | head JIT | x |")
print("|---|---|---|---|---|---|")
xb, xh = [], []
for b in B:
    i = st.median(d[(b, "interp32")])
    ob, oh = st.median(d[(b, "base32")]), st.median(d[(b, "head32")])
    xb.append(ob / i)
    xh.append(oh / i)
    print(f"| {b} | {i:g} | {ob:g} | {ob / i:.2f} | {oh:g} | {oh / i:.2f} |")
print(f"| geomean | | | {g(xb):.2f} | | {g(xh):.2f} |")
if bad:
    print("BAD ROWS:", bad)
print("samples per cell:", sorted(set(len(v) for v in d.values())))
```

- [ ] **Step 3: Run Octane**

With nothing else building or testing:

```bash
bash $S/hv32impl/bench.sh
python3 -I $S/hv32impl/summ.py $S/hv32impl/bench
```

Expected (spec criterion 4): HV64 no benchmark beyond noise; HV32
NavierStokes, Crypto and Box2D faster beyond noise; 5 samples per cell, no
BAD ROWS. Compare the HV32 JIT/interp column with the 2026-10-28 table in
dz 01a13c62 (NavierStokes 1.52x, Crypto 1.85x, Box2D 1.25x at 7357a443f).
Results that miss expectations are reported as measured, with a profile
(Step 5), not tuned away.

- [ ] **Step 4: Counters and code size**

Counters, from the clean binaries:

```bash
P=$S/hv32impl/bench
mkdir -p $P/counters $P/size
B="richards deltablue crypto raytrace earley-boyer splay navier-stokes pdfjs mandreel gbemu box2d zlib typescript"
for c in base64:$S/hv32impl/base/cmake-build-jit2 \
         head64:cmake-build-jit2/bin \
         base32:$S/hv32impl/base/cmake-build-jit2-hv32 \
         head32:cmake-build-jit2-hv32/bin; do
  n=${c%%:*}; bin=${c#*:}
  for b in $B; do
    taskset -c 2 $bin/hermes -Xjit -Xjit-emit-counters $P/hbc/$n/$b.hbc \
      > $P/counters/$n.$b.txt 2>&1
  done
done
grep -H -E '^(NumPutByValBoxes|NumPutByIdBoxes|NumStoreBoxFallbacks|NumRecompileChecks|NumRecompiles):' $P/counters/*.txt
```

Report per benchmark and mode, base and head, the five counters (base has
no box counters: "n/a").

Code size is each compiled body's own emitted bytes, `em_.code.codeSize()`,
which the temporary line of `$S/hv32impl/codesize.diff` prints as `JIT code
size (bytes): N` just before each `JIT successfully compiled` line (Task 1
Step 1 explains why the cumulative `JIT total memory usage` cannot give
it). The base copies were built in Task 1 Step 1; build the head ones the
same way, never committing the line:

```bash
git apply $S/hv32impl/codesize.diff
mkdir -p $S/hv32impl/head-codesize
for t in cmake-build-jit2 cmake-build-jit2-hv32; do
  cmake --build $t --target hermes
  cp $t/bin/hermes $S/hv32impl/head-codesize/$t-hermes
done
git checkout -- lib/VM/JIT/JitCompiler.cpp
git status --short
for t in cmake-build-jit2 cmake-build-jit2-hv32; do
  cmake --build $t --target hermes
done
```

`git status --short` must list only the two untracked files of Global
Constraints. Then, with `sz` printing one `size<TAB>compile line` per body:

```bash
sz() {
  awk '/^JIT code size \(bytes\): / {s = $5}
       /^JIT successfully compiled / {print s "\t" $0}'
}
for c in base64:$S/hv32impl/base/cmake-build-jit2/hermes-codesize \
         head64:$S/hv32impl/head-codesize/cmake-build-jit2-hermes \
         base32:$S/hv32impl/base/cmake-build-jit2-hv32/hermes-codesize \
         head32:$S/hv32impl/head-codesize/cmake-build-jit2-hv32-hermes; do
  n=${c%%:*}; h=${c#*:}
  for b in $B; do
    $h -Xjit -Xjit-max-recompiles=0 -Xdump-jitcode=2 $P/hbc/$n/$b.hbc 2>&1 |
      sz > $P/size/$n.$b.first.txt
    $h -Xjit -Xdump-jitcode=2 $P/hbc/$n/$b.hbc 2>&1 |
      sz > $P/size/$n.$b.all.txt
  done
  $h -fno-inline -Xjit=force -Xdump-jitcode=2 \
    test/jit/x86-64/putbyval-inline-emitted.js | sz | grep "'store'" \
    > $P/size/$n.site-putbyval.txt
  $h -fno-inline -Xjit -Xjit-threshold=4 -Xdump-jitcode=2 \
    test/jit/x86-64/putbyid-inline-emitted.js | sz | grep "'setIndirect'" \
    > $P/size/$n.site-putbyid.txt
done
for f in $P/size/*.first.txt $P/size/*.all.txt; do
  echo "$(basename $f): $(awk -F'\t' '{t += $1} END {print t, NR}' $f)"
done
cat $P/size/*.site-*.txt
```

Report, per benchmark, the summed bytes and body count with recompilation
off (the same first versions on both sides) and on, base and head, HV32
and HV64; and the bytes of `store` (one PutByVal JSArray site) and
`setIndirect` (one PutById site), base and head, with the per-site
difference. With recompilation off, every HV64 body compiled in both runs
must have the same size in base and head (criterion 2 again, on Release;
a body that only one timed run reached is listed, not compared):

```bash
for b in $B; do
  python3 -I - $P/size/base64.$b.first.txt $P/size/head64.$b.first.txt <<'PY'
import sys
def load(f):
    return dict(reversed(l.rstrip("\n").split("\t", 1)) for l in open(f))
a, b = load(sys.argv[1]), load(sys.argv[2])
both = a.keys() & b.keys()
print(sys.argv[1].split("/")[-1], "common", len(both),
      "differ", sum(a[k] != b[k] for k in both),
      "only-base", len(a.keys() - b.keys()),
      "only-head", len(b.keys() - a.keys()))
PY
done
```

Expected: `differ 0` for every benchmark.

- [ ] **Step 5: Profiles**

```bash
P=$S/hv32impl/bench
for c in base32:$S/hv32impl/base/cmake-build-jit2-hv32 head32:cmake-build-jit2-hv32/bin; do
  n=${c%%:*}; bin=${c#*:}
  for b in navier-stokes crypto; do
    perf record -F 2000 -o $P/perf-$n-$b.data -- \
      taskset -c 2 $bin/hermes -Xjit $P/hbc/$n/$b.hbc
    perf report -i $P/perf-$n-$b.data --stdio --no-children --sort dso \
      2>/dev/null | grep -E '%' | head -5 > $P/perf-$n-$b.dso.txt
    perf report -i $P/perf-$n-$b.data --stdio --no-children --sort sym \
      2>/dev/null | grep -E 'putByValWithReceiver_RJS|_setOwnIndexedImpl|_haveOwnIndexedImpl|_sh_ljs_put_by_val|HadesGC' \
      > $P/perf-$n-$b.sym.txt
  done
done
```

Report, per benchmark, base and head: the JIT-code share (`[JIT]` rows of
the dso report) and the shares of `putByValWithReceiver_RJS`,
`ArrayImpl::_setOwnIndexedImpl`, `ArrayImpl::_haveOwnIndexedImpl` and the
GC.

- [ ] **Step 6: The boxing share of helper calls (criterion 1)**

"Before" is the dz 01a185d0-5d43 2026-10-29 log entry, measured with this
instrumentation on 9f8383072, whose `lib/` and `include/` equal BASE's
(Task 1 Step 1). Re-measure "after" on the head tree; the instrumentation
is never committed:

```bash
cp $S/hv32diag/instrumentation.diff $S/hv32impl/
git apply --check $S/hv32impl/instrumentation.diff &&
  git apply $S/hv32impl/instrumentation.diff
```

(If `--check` fails, apply its hunks by hand; they add code only.) In
`lib/VM/JIT/JitHandlers.cpp`, in `hv32diag::Table::~Table()`, make the
ValueBox breakdown a total: after `uint64_t tot = 0, totHits = 0,
totFirst[NumReasons] = {};` add `uint64_t totRest[NumReasons] = {};`;
inside the `for (auto &kv : sites)` loop's reason loop add
`totRest[r] += kv.second.rest[r];`; and after the loop that prints
`HV32DIAG   total %s` add

```cpp
    for (unsigned r = 0; r < NumReasons; ++r)
      if (totRest[r])
        fprintf(
            stderr,
            "HV32DIAG   total ValueBox then %s: %llu\n",
            kReasonNames[r],
            (unsigned long long)totRest[r]);
```

Then:

```bash
cmake --build cmake-build-jit2-hv32 --target hermes
for b in navier-stokes crypto; do
  taskset -c 2 cmake-build-jit2-hv32/bin/hermes -Xjit -Xjit-emit-counters \
    -gc-print-stats $S/hv32impl/bench/hbc/head32/$b.hbc \
    > $S/hv32impl/diag-head-$b.txt 2>&1
  grep -E 'HV32DIAG (total|  total)|NumStoreBoxFallbacks|"Num young gen collections"' \
    $S/hv32impl/diag-head-$b.txt
done
git checkout -- lib/VM/JIT/JitCompiler.cpp lib/VM/JIT/JitHandlers.cpp \
  lib/VM/JIT/JitHandlers.h lib/VM/JIT/x86-64/JitEmitter-property.cpp
git status --short
cmake --build cmake-build-jit2-hv32 --target hermes
```

`git status --short` must show only the two untracked files of Global
Constraints. Expected, per benchmark: `total ValueBox then Unexplained`
(PutByVal helper calls with a boxing value that every guard and barrier
check would have let through: since the box arm, only young-generation-full
fallbacks) is at most `NumStoreBoxFallbacks` (which also counts the PutById
tier's), which is at most the number of young collections; every other
ValueBox call is `BarrierMarking`, `BarrierCompacting` or a failed guard.
Report the totals against the dz "before" (NavierStokes 63.7M helper calls,
95.4% ValueBox; Crypto 96.2M, 99.1%). If `then Unexplained` exceeds
`NumStoreBoxFallbacks`, a box store still declines for a reason the plan
did not foresee: stop and report.

- [ ] **Step 7: Delivered section**

Append to the end of the spec, with the date filled in:

```markdown
## Delivered (YYYY-MM-DD)

Commits `<first>..<last>` on branch `x86-jit` (base `<BASE>`).

### Differences from the spec
(one bullet per spec resolution in the plan's last section, plus anything
the implementation changed, each with its reason)

### Matrix
(table: tree | what ran | result -- the full check-hermes on the three
x86-64 trees; the JIT suite and qemu-sanity on the three arm64 trees; the
gc-check.py summaries; the HV64 identity runs, controls and sensitivity
checks; then each mutation and what it failed)

### Benchmarks
(the exact commands; the two base/head tables with geomeans; the HV32
JIT/interp table beside the 2026-10-28 ratios of dz 01a13c62; the counters
table; code size per benchmark and per site; the profile shares)

### Helper calls
(Step 6: before and after per benchmark: PutByVal helper calls, ValueBox
calls and their breakdown, NumStoreBoxFallbacks, young collections)
```

Every value comes from a run made in this task or a task report; nothing
is estimated.

- [ ] **Step 8: dz**

Box2D, on its own issue:

```bash
dz comment 01a04e00-0811 -m - --json <<'EOF'
<date>, x86-64 Release, from bytecode, taskset -c 2, median of 5: the
store tiers now box doubles inline under HV32 (spec
2026-10-29-jit-inline-boxed-double-store-design.md, "Delivered"). Box2D
JIT <base median> -> <head median> (<ratio>); HV32 JIT/interp <x.xx>
(1.25 on 2026-10-28; HV64 1.92). NumPutByIdBoxes <n>,
NumStoreBoxFallbacks <n>, NumRecompileChecks <before> -> <after>.
<What remains, from the profile.>
EOF
```

The marking residue, as its own issue (spec "Non-goals": "A separate
change, measured after this one"):

```bash
dz add --type task --component jit -m - --json \
  "JIT: inline store tiers decline every old-target store while old-gen marking or a compaction is active" <<'EOF'
emitSafeStoreOrSlow declines an old-target store to the helper whenever
the old-generation marking barriers are on or a compactee exists
(x86-64 JitEmitter-internal.cpp, the predicate's steps 2-3). With boxing
inline (spec 2026-10-29-jit-inline-boxed-double-store-design.md), these
are most of what is left of NavierStokes' and Crypto's PutByVal helper
calls under HV32: <Step 6 BarrierMarking/BarrierCompacting counts and
shares>. The spec named this a separate change, measured after the box
arm.
EOF
```

Then dz 01a185d0-5d43: if Step 6 met criterion 1 (no boxing helper call
but the fallbacks), close it:

```bash
dz close 01a185d0-5d43 --as fixed -m - --json <<'EOF'
Fixed by boxing doubles inline in both store tiers (spec
2026-10-29-jit-inline-boxed-double-store-design.md, "Delivered"),
<commits>. HV32, from bytecode, median of 5: NavierStokes JIT
<before> -> <after> (JIT/interp 1.52x -> <x>), Crypto <before> ->
<after> (1.85x -> <x>). Generic PutByVal path: NavierStokes
<share before> -> <after>, Crypto <before> -> <after> of samples.
Boxing helper calls: only young-generation-full fallbacks remain
(<n>). The marking/compaction declines that remain are dz <new id>.
EOF
```

Otherwise `dz comment 01a185d0-5d43 -m - --json` with the same numbers and
what failed, and leave it open. Every `<...>` is a measured value.

- [ ] **Step 9: Commit**

```bash
git add doc/superpowers/specs/2026-10-29-jit-inline-boxed-double-store-design.md \
  dz/issues/01a185d0-5d43-700c-a5b9-5868c8a9de62.md \
  dz/issues/01a04e00-0811-70c9-809b-0822bf587426.md \
  dz/issues/<new id>.md
git commit   # "JIT: record the delivered inline boxed-double measurements"
```

(`dz add` prints the new issue's id in its JSON; stage that one file by
path.)

---

## Spec resolutions (summary)

1. "The HV64 pins are not edited": two directives shared by every mode
   cannot hold for the new boxed-mode layout (the young label now precedes
   the store; the card block falls through into `done`). They are
   re-prefixed `SPEC-HV64` (x86-64 putbyid: `[[YOUNG]]:`; arm64 putbyid:
   `b [[DONE]]` and `[[YOUNG]]:`), and the object-check lines of the HV32
   and BOXED prefixes become label definitions. Every directive active
   under HV64 stays verbatim and in order, so the HV64 check sequence is
   unchanged; HV64 code identity is shown separately (Tasks 1, 2, 4).
2. Layout: `slowFull` sits after the inline path's unconditional `jmp
   dirtyCard` and before `boxLab`, as the spec's "out of line, after an
   unconditional jump" requires; the box arm falls into the card block,
   which falls into `done`.
3. `t3` is allocated after each tier's existing temporaries, so the
   registers of the others are those of the default mode.
4. Stale comments the spec's list does not name but this change makes
   stale are updated too: the GetByVal tiers' mention of the deleted
   wrapper (x86-64 JitEmitter-property.cpp:1621, arm64 :1721), the arm64
   PutById xScratch comment (:2566), and the "encode comes first" headers
   of the four pin files.
5. Tests beyond the spec's list (Review Focus): the card test's
   out-of-line-slot case, and `store-boxed-double-values.js` (edge values,
   both tiers, a duo site). The counting tests also run with
   `-Xdump-jitcode=2` to verify the environmental assumption that nothing
   compiles inside the measured region.
6. Interim state: Task 1's five tests are x86-64 only until Task 2 lifts
   the gate.
7. Spec criterion 2's dump comparison is strict: raw dumps from runs with
   ASLR off, equal byte for byte except immediates or pool words that `nm`
   resolves to the same sized symbol and offset in both binaries, every
   such value resolved on both sides even when the numbers are equal (a
   stale address is reported); only the benchmarks' timed score line is
   dropped. It covers the four pin inputs,
   NavierStokes and Box2D with `-Xjit-max-recompiles=0`, and on x86-64
   every JIT test, on arm64 HV64 as well as x86-64, with stability and
   sensitivity controls; Release HV64 body sizes are compared too.
8. Criterion 1's "dz 01a185d0-5d43 instrumentation" is
   `$S/hv32diag/instrumentation.diff` from the root-cause session, with
   one addition (a total of the ValueBox breakdown); its "before" numbers
   are the dz log entry, measured on code identical to BASE.
9. Closing dz 01a185d0-5d43 files the marking/compaction residue as a new
   issue, which the spec names a separate change.
10. GC conditions are checked from `-gc-print-collection-stats` records
   (old-generation starts, including uncompleted ones, completed
   collections, and peak allocated plus external bytes), and the exact
   counts are named as the direct evidence for the measured stores; no
   stats field exposes the marking flag itself.
11. Code size is measured per compiled body with a temporary, never
   committed `JIT code size` dump line (`codesize.diff`), applied to BASE in
   Task 1 Step 1 and to head in Task 4 Step 4, each revert checked with
   `git status`.

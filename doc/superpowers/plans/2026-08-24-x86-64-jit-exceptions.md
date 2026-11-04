# x86-64 JIT Exceptions, Switches, Iterators, Strings (Milestone 5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Empty the stub file: all 31 remaining emitters ported, reaching
arm64's full opcode surface (only `AsyncBreakCheck` stays permanently
unsupported, as on arm64). The headline gate: `aarch64/jit-stress.js`
runs under `-Xjit=force -Xjit-crash-on-error` natively, in all three heap
modes, with and without type asserts, matching the interpreter.

**Architecture:** Three port tasks — strings/typeof/misc (Task 1),
exceptions (Task 2: `catchInst`, throws, `emitCatchTable`, removing
`leave()`'s catch-table decline), switches/iterators/arguments/for-in
(Task 3) — then the milestone gate + docs (Task 4).

**Tech Stack / Spec:** as milestone 4 (three ASan builds; spec milestone
5). The rsp-delta counter, free-after-call invariant, allocation
preconditions, and GC pinning discipline all BIND.

## Global Constraints

Same as milestone 4, plus the inherited obligations, each MANDATORY:

- **Four test headers become false when strings/exceptions land**
  (objects.js, props.js, hvmodes.js, arrays.js — their "X still
  declines, so global stays interpreted as the oracle" rationales).
  Task 1 re-reads and rewrites all four to match reality, re-measuring
  which functions compile after each task's landing (final re-check in
  Task 4).
- **preserve_input's fragile coverage**: props.js's warm-cache SPEC RUN
  lines are the only coverage for `emit_sh_cp_decode_non_null_preserve_
  input`; after strings land, re-verify threshold mode still leaves the
  warm-up interpreted (Task 1 gate).
- **bumpAllocAndUnpoison's ASan pushes** are transient rsp movement
  outside the rspDelta_ counter's scope — Task 2 (which touches the rsp
  story) either folds them into the counter or states the scope split in
  the counter's comment.
- **Try-functions have zero global registers** (the RegAlloc.cpp
  contract, asserted in enter()) — Task 2's emitters may RELY on it but
  must not weaken the assert.
- **jmpUndefined finally becomes reachable** (for-in) — Task 3 pins it.

---

### Task 1: Strings, typeof, and stragglers

**Files:** `lib/VM/JIT/x86-64/JitEmitter-const.cpp` (loadConstString,
loadConstBigInt), `JitEmitter-arith.cpp` (typeOf, addS, addEmptyString),
`JitEmitter-control.cpp` (typeOfIs, jmpTypeOfIs, jmpBuiltinIs),
`JitEmitter.cpp` (unreachable, profilePoint, directEval, debugger,
createRegExp — wherever arm64 has them), `JitEmitter-array.cpp`
(toPropertyKey), headers/stubs/CMake as needed.
Create: `test/jit/x86-64/strings.js`, `test/jit/x86-64/typeof.js`.
Modify: the four stale test headers.

Port sources: the arm64 functions of the same names (grep arm64 tree;
loadConstString's x86 helper `loadConstStringInGpX` already exists from
milestone 4 — the emitter wraps it). typeOfIs/jmpTypeOfIs carry
`TypeOfIsTypes` bit-test logic — port the structure exactly; the tag
helpers all exist. `jmpBuiltinIs` uses `emit_load_builtin_closure`
(exists since milestone 3).

Tests: strings.js (string consts incl. long/short, `+` concat via addS
with both-string fast path and mixed slow paths, addEmptyString via
`"" + x` patterns — check what actually emits it, template-literal-free);
typeof.js (all typeof results, typeOfIs/jmpTypeOfIs polarity via
`typeof x === "string"` branch patterns, jmpBuiltinIs if reachable —
check what lowers to it and note honestly if nothing does). Standard RUN
pattern; NOW ADD `-Xjit-crash-on-error` to the compile-status RUNs of
any test whose every function compiles (measure first). Re-measure and
rewrite the four stale headers. Gates + prove-can-fail (break addS's
both-string fast path type check — strings.js must fail). Commit:
"JIT: x86-64 strings, typeof and remaining scalar emitters".

---

### Task 2: Exceptions

**Files:** `JitEmitter-control.cpp` (catchInst, throwInst,
throwIfEmptyUndefinedImpl, throwIfThisInitialized — arm64
JitEmitter-control.cpp:18-139), `JitEmitter.cpp` (port emitCatchTable
from arm64 JitEmitter.cpp:949-985; REMOVE leave()'s
`unsupported("catch table")` decline and emit the real path — the
prologue's SHJmpBuf/setjmp machinery has been in place since milestone
1), stubs/headers.
Create: `test/jit/x86-64/exceptions.js`.

Key correctness points (from the doc and findings history, restate in
code comments where arm64 does):
- Try-functions have no global registers (longjmp restores callee-saved
  regs to setjmp-time values — a global would present a stale value to a
  catch handler); every FR's canonical home is the frame. enter()'s
  assert stays.
- catchInst restores SHLocals from the frame area ([rsp+...] — the
  rspDelta_ counter asserts ==0 at instruction boundaries; catchInst IS
  an instruction boundary, document the interaction). Resolve the
  bumpAllocAndUnpoison scope note here (fold or document).
- The catch table maps handler bytecode targets to code labels; the
  shared driver already collects exceptionHandlers_ — your leave() wires
  them through emitCatchTable exactly as arm64.
- SHJmpBuf is 16-byte aligned unconditionally (milestone-1 review fix) —
  the assumption arm64 never needed; note it satisfied.

Test exceptions.js: try/catch/finally, throw across compiled frames
(thrower/catcher both compiled), catch-in-loop, rethrow, nested try,
throwIfEmpty via TDZ patterns (`let` before init — check what compiles),
type-assert RUN (catch paths under asserts). Under crash-on-error where
full coverage holds. Prove-can-fail: corrupt the catch-table target
resolution (off-by-one the handler index) — exceptions.js must fail
loudly. Commit: "JIT: x86-64 exceptions".

---

### Task 3: Switches, iterators, arguments, for-in

**Files:** `JitEmitter-control.cpp` (uintSwitchImm — arm64
JitEmitter-control.cpp:495-593 — jump table in RO data, uses
emit_double_is_uint32 [remember the jp obligation]; stringSwitchImm —
:594-624 — the runtime table + the shared driver's post-compile fixup
already exists driver-side), `JitEmitter-array.cpp` (getArgumentsLength,
getArgumentsPropByValImpl, reifyArgumentsImpl, iteratorBegin/Next/Close,
getPNameList, getNextPName — arm64 JitEmitter-array.cpp:249-604),
stubs/headers.
Create: `test/jit/x86-64/switches.js`, `test/jit/x86-64/forin.js`,
`test/jit/x86-64/args.js`.

Notes: uintSwitchImm's jump table is label-addresses in RO data — on
x86 use RIP-relative lea + indexed load + indirect jmp (or the exact
arm64 structure translated; document). jmpUndefined gets its FIRST
runtime coverage via for-in — add an explicit forin.js case whose
correctness depends on it and SAY SO in the header. iterator* are mostly
runtime calls; getNextPName has a real fast path (property enumeration).
Tests cover: dense uint switch (all edges: min, max, default, dense
holes), string switch (hit/miss), for-in over objects incl. mid-loop
mutation patterns from jit-stress, arguments.length/arguments[i]
reified and not, spread-free. Prove-can-fail: break the jump-table
scale — switches.js must fail. Commit: "JIT: x86-64 switches,
iterators, arguments and for-in".

---

### Task 4: The milestone gate, docs, arm64 check

- [ ] **Stub audit**: JitEmitter-stubs.cpp must now contain ONLY
  AsyncBreakCheck-equivalent permanent declines exactly matching arm64's
  EMIT_UNIMPLEMENTED set (compare; list any residue and why).
- [ ] **The jit-stress gate**: `aarch64/jit-stress.js` under
  `-Xjit=force -Xjit-crash-on-error` on HV64, HV32, BOXED, each
  diffed against the same binary's interpreter run, plus a
  `-Xjit-emit-type-asserts` variant on HV64. All identical, ASan clean,
  zero declines (dump status to prove). If ANY function still declines,
  name the opcode — that is a milestone-5 bug, fix it (AsyncBreakCheck
  aside; check jit-stress does not contain one).
- [ ] Add `test/jit/x86-64/stress.js` as a standing lit gate: RUN lines
  running jit-stress.js? No — lit tests are self-contained; instead
  copy the jit-stress body into stress.js (attribute the origin in the
  header) with the standard differential + crash-on-error RUNs.
- [ ] **The 497 sweep** on all three modes with the capacity-scaling
  method for stack-depth files; report compiled-function file count
  (expect a jump from ~270).
- [ ] **arm64 insurance**: dumps identical vs baseline + 46 passes,
  PASTED output (the evidence standard is now twice-litigated — paste
  from the first run).
- [ ] doc/JIT.md milestone-5 status rewrite: full opcode surface,
  what remains (goldens/CI = milestone 6; AsyncBreakCheck permanent).
- [ ] Commit: "JIT: x86-64 milestone-5 gate" (tests+doc; any fixes as
  their own commits first).

---

## Out of scope

Goldens, CI wiring, test/jit un-gating, perf pass (milestone 6). eval's
FULL semantics (directEval is a runtime call like arm64's — port is in
scope, deep eval testing is not).

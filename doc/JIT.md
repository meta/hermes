# The Hermes JIT (arm64)

This document describes the architecture of the existing arm64 JIT, as a
reference for maintaining it and for porting it to x86-64. It was produced by
studying the code on branch `x86-jit` (based on `static_h`) and has been
revised for the 2026-08 refactor series (the `JitEmitter.cpp` file split,
thunk removal, capture-based slow paths, and encoding-fallback fixes). Source
references in the body are to the post-refactor tree; the findings appendix
deliberately keeps its references to the pre-fix revisions it reviewed.

## Overview

The JIT is a simple, single-pass baseline compiler: it translates HBC bytecode
directly to native code, one instruction at a time, with no IR and no
scheduling. Its performance comes from:

- keeping frame registers in machine registers (a static "global" assignment
  for number/non-pointer registers plus per-basic-block "local" temporaries),
- inline fast paths for the common cases (arithmetic on doubles, property
  access through inline caches, young-gen bump allocation), with out-of-line
  slow paths that call the same C runtime helpers the Static Hermes native
  backend uses (`_sh_ljs_*`), and
- reusing the SH ABI: a JIT-compiled function is a drop-in replacement for a
  bytecode CodeBlock, called via `JITCompiledFunctionPtr` =
  `HermesValue (*)(Runtime *)`.

The machine-code emitter is [asmjit](https://asmjit.com) (vendored in
`external/asmjit`; the x86 backend is already present in the vendored copy).

### Source layout

| File | Role |
|---|---|
| `include/hermes/VM/JIT/Config.h` | Decides `HERMESVM_JIT` from `HERMESVM_ALLOW_JIT` + platform: arm64, or x86-64 SysV (`__x86_64__`/`_M_X64`, non-Windows), each requiring no compressed pointers or a contiguous heap. On Apple platforms the JIT is additionally disabled *except* on macOS and Mac Catalyst, so arm64 macOS is a supported (and convenient) host for testing the JIT. Also defines the per-arch selectors `HERMESVM_JIT_ARM64`/`HERMESVM_JIT_X86_64` (exactly one set when the JIT is enabled), which `JIT.h` and arch-specific sources dispatch/guard on. |
| `include/hermes/VM/JIT/JIT.h` | Dispatches to the per-arch `JITContext` via `HERMESVM_JIT_ARM64`/`HERMESVM_JIT_X86_64`, including `hermes/VM/JIT/arm64/JIT.h` or `hermes/VM/JIT/x86-64/JIT.h` accordingly; provides a no-op `JITContext` when the JIT is disabled. |
| `include/hermes/VM/JIT/arm64/JIT.h` | Public `JITContext`: enablement, thresholds, `shouldCompile`/`compile`, counters, `markRoots`. |
| `lib/VM/JIT/JitCompiler.cpp` | `JITContext::Compiler`: per-opcode `emitXXX` methods that decode operands and forward to the `Emitter`. Drives BB-by-BB compilation. Arch-independent; compiled inside the arch namespace selected by `JitCurArch.h`. |
| `lib/VM/JIT/arm64/JIT.cpp` | `JITContext` housekeeping: construction, `setHCIdLimit`, `dumpCounters`, `markRoots`. |
| `lib/VM/JIT/arm64/JitEmitter.h` | The `Emitter` class definition: the FR/HWReg model, `TempRegAlloc`, the capture-based `SlowPath` class, and the full emitter method surface (which is effectively the porting contract). |
| `lib/VM/JIT/arm64/JitEmitter.cpp` | Emitter lifecycle: constructor, `enter`/`frameSetup`/`leave` (prologue/epilogue), `newBasicBlock`, the runtime-call plumbing (`callRuntime*`), slow-path/catch-table/RO-data emission, `initHCLazyIDMayAlloc`, `addToRuntime`. |
| `lib/VM/JIT/arm64/JitEmitter-regalloc.cpp` | The register-file engine: `getOrAllocFRIn*`, `movHW*`, sync/spill/free. |
| `lib/VM/JIT/arm64/JitEmitter-internal.{h,cpp}` | Shared emission helpers: the `emit_sh_ljs_*` tag/encoding helpers, `emit_load_from_base_offset`, the SHV decoder, `EMIT_RUNTIME_CALL*` macros (`.h`); cold non-member helpers, object/environment init, asmjit error handler and logger (`.cpp`). |
| `lib/VM/JIT/arm64/JitEmitter-{alloc,const,object,property,call,env,array,arith,control}.cpp` | The per-instruction emitters, split by topic: young-gen allocation; constant materialization; object construction; the property protocol (incl. `GetByIdImpl`); JS calls; environments/closures; arrays/iterators/arguments; arithmetic and comparisons; control flow, switches and throws. See `doc/superpowers/specs/2026-08-18-jitemitter-split-design.md` for the split rationale and boundaries. |
| `lib/VM/JIT/arm64/JitImpl.h` | `JITContext::Impl`: the asmjit `JitRuntime` (owns executable memory), HC lazy-ID counter, and `usedHCs` root array. |
| `include/hermes/VM/JIT/x86-64/JIT.h`, `lib/VM/JIT/x86-64/*` | The x86-64 backend: 16 files under `lib/VM/JIT/x86-64/` (`JIT.cpp`, `JitEmitter.{h,cpp}`, `JitEmitter-regalloc.cpp`, `JitEmitter-internal.{h,cpp}`, `JitEmitter-{alloc,arith,array,call,const,control,env,object,property}.cpp`, `JitImpl.h`) plus the public header, mirroring the arm64 rows above file-for-file with the same class/method surface. See "The x86-64 backend" below for the arch-specific design (register budget, condition-code mapping, tag encoding, and so on). |
| `lib/VM/JIT/JitHandlers.{h,cpp}` | C++ helpers callable from emitted code that are JIT-specific (the generic ones are the `_sh_ljs_*` functions from the SH runtime). Arch-independent. |
| `include/hermes/VM/JIT/JitCounters.h` | The `JIT_COUNTERS` list and `JitCounter` enum. The counter array is ABI between the VM and emitted code; arch-independent. |
| `lib/VM/JIT/DiscoverBB.cpp` | Scans bytecode to find basic-block boundaries (branch targets, fallthroughs after branches, Catch, switch tables, exception handler targets). Arch-independent. |
| `lib/VM/JIT/RuntimeOffsets.h` | `offsetof` constants for fields the emitted code touches directly (Runtime, StackOverflowGuard, CodeBlock, JSFunction, HiddenClass, IdentifierTable, Hades young-gen fields...); it also carries the card-table/segment/compactee geometry the inline write barrier reads, each offset or constant static_assert-pinned against the GC (see "Inline write barrier" below). Arch-independent, and read by both backends. |
| `lib/VM/JIT/PerfJitDump.cpp` | Linux `perf` jitdump support. Arch-independent. |
| `doc/JITTesting.md` | The CI-shaped build/test matrix for all seven JIT configs (arm64-qemu HV64/HV32/BOXED, x86-64 HV64/HV32/BOXED ASan+Debug, x86-64 Release), the dump-baseline workflow summary, the release-build validation writeup, and the perf sanity table. Companion to this file and `utils/jit/README.md`. |

### Tiering / when compilation happens

`JITContext::shouldCompile()` is checked by the interpreter (a) when entering a
function body in `CASE(Call)`-style dispatch and (b) in `interpretFunction`
entry (`lib/VM/Interpreter.cpp` around lines 563 and 1279). A function is
compiled when its execution count reaches
`defaultExecThreshold_ >> (loopDepth * 2)` (default threshold 32, loop depth
capped at 3, so hot loops compile at count 0), unless `getDontJIT()` is set or
JIT is disabled. `forceJIT` compiles everything on first call.

Compilation failure paths:

- Unsupported instruction or asmjit error: the compiler `longjmp`s out
  (`_sh_longjmp` on `errorJmpBuf_`), sets `setDontJIT(true)` on the CodeBlock
  and returns nullptr (or `hermes_fatal` if `crashOnError`). The only
  currently-unsupported opcode is `AsyncBreakCheck` (plus anything new that
  hasn't been implemented).
- Memory limit (default 32 MB of emitted code): compilation succeeds or is
  discarded, and the JIT disables itself globally (`enabled_ = false`).

Compiled code is never freed (the `JitRuntime` holds it for the lifetime of
the `JITContext`); consequently GC objects referenced by emitted code must be
permanently rooted (see "GC integration" below).

## The Emitter

### Frame register (FR) model

Bytecode operates on frame registers r0..rN. The JIT gives every FR a fixed
home slot in the VM register stack frame (`xFrame + (FirstLocal + i) * 8`),
and additionally may cache it in machine registers. Per FR
(`FRState` in `JitEmitter.h`):

- **globalReg**: a callee-saved register statically assigned for the whole
  function in `Emitter::enter()`. The SHBC "number register count" and
  "non-pointer register count" function header fields (computed by the
  compiler's register allocator, which sorts number/non-pointer registers
  first) determine eligibility: number FRs prefer VecD (d8–d15), non-pointer
  FRs prefer GpX (x21–x28), overflowing into the other class while registers
  remain. Pointer-typed FRs never get a global register (they must be visible
  to the GC in the frame).
- **localGpX / localVecD**: temporary registers (caller-saved: x0–x15,
  d0–d7/d16–d31) holding the value within the current basic block. Allocated
  LRU (`TempRegAlloc` + `SimpleLRU`) with spilling.
- **Invariants** (documented at `FRState`): local regs, if any, hold the
  latest value; a global reg holds the latest value iff `globalRegUpToDate`;
  the frame slot holds the latest value iff `frameUpToDate`; "global reg stale
  while frame fresh" is not a legal state.
- **Types**: `FRType` (Number / Bool / OtherNonPtr / Pointer and unions).
  `globalType` holds for the whole function (from the register counts);
  `localType` can be narrowed within a BB by emission (e.g. after `add` the
  result is known Number) and is reset to `globalType` at BB entry. Known
  types let later instructions skip tag checks — a wrong `localType`
  assignment anywhere is a correctness bug elsewhere.

Key operations: `getOrAllocFRInGpX/InVecD/InAnyReg(fr, load)` (get a register
holding the FR, optionally loading the current value), `frUpdatedWithHW`
(declare that a register now holds the FR's new value), `syncAllFRTempExcept`
+ `freeAllFRTempExcept` (spill/drop all temps — required before any C++ call
since temps are caller-saved), `syncToFrame` (make the memory slot valid so
its address can be passed to a helper), `syncFrameOutParam` (a helper wrote
the frame slot in memory; reload global reg from it).

At every basic-block boundary (`newBasicBlock`) all temps are synced and
freed, so cross-BB state lives only in the frame and global regs. Note the
subtlety at `newBasicBlock`: for FRs with a global reg, `frameUpToDate` is
reset to false (the global reg is the canonical copy); the code relies on
BB-entry state being consistent regardless of predecessor.

### Machine register convention (arm64)

- x19 = `xRuntime` (Runtime*), x20 = `xFrame` (current frame base), both saved
  in the prologue.
- x21 doubles as the return-value stash: `ret()` moves the FR into x21, jumps
  to the shared `leave` label, which moves x21 → x0 (x21 is always saved even
  if no FR global uses it).
- x0–x15: temporaries / argument registers for helper calls. x16/x17 are
  scratch *outside the allocator's domain*: call-target and IP
  materialization, and the frame-offset fallback in `_loadFrame`/
  `_storeFrame` when a slot offset does not encode. Any future
  emitted-code instrumentation (e.g. the type-assert design) should use
  these, never `allocTempGpX`, to keep assert builds allocation-identical.
- Helper calls use the standard AAPCS64 ABI; emitted code passes Runtime* in
  x0, usually `xFrame` or FR addresses (`loadFrameAddr`) as pointer args.

### Function prologue (`enter` → `frameSetup`)

Native stack layout (grows down): optional `SHJmpBuf` + saved `SHLocals*`
(only if the function has an exception table), then saved x19..x2x pairs,
saved d8..d1x pairs, then x29/x30 at the top; x29 points at the saved-fp pair.
The prologue:

1. Saves callee-saved registers actually used (+x19/x20/x21 always).
2. `xRuntime = x0`.
3. If there are try regions: saves `runtime.shLocals` into the native frame.
4. Native stack overflow check against
   `Runtime::overflowGuard_.nativeStackHigh/Size`, slow path calls
   `_sh_check_native_stack_overflow` (no IP save — the exception belongs to
   the caller).
5. `xFrame = runtime.stackPointer`.
6. `ProhibitInvoke` check (from the function header flags): loads NewTarget
   from the *incoming* frame and throws invalid-call/invalid-construct via a
   slow path.
7. Register-stack overflow check + bump: allocates
   `frameSize + FirstLocal` slots, stores new `stackPointer` and
   `currentFrame`, then zero-fills the newly allocated slots (vectorized,
   loop for >32 regs). Zero is a "raw" value ignored by the GC (HV64
   invariant).
8. If there are try regions: pushes an `SHJmpBuf` (links `shr->shCurJmpBuf`)
   and calls `_sh_setjmp`; a non-zero return branches to the catch-table
   dispatch code (see Exceptions).

The epilogue (`leave`) undoes this in reverse (pops jmpbuf, restores
`stackPointer`/`currentFrame` from `xFrame` and the frame's PreviousFrame
slot, restores saved regs, `ret`).

### VM stack frame layout

From `sh_stack_frame_layout.h` (slot indexes relative to the frame pointer):
locals are at +1.., and the caller-populated metadata sits below:
PreviousFrame(-1), SavedIP(-2), SavedCodeBlock(-3), SHLocals(-4),
ArgCount(-5), NewTarget(-6), CalleeClosureOrCB(-7), ThisArg(-8),
FirstArg(-9)...

An outgoing call is set up by writing to FRs *above* the current frame
(`nRegs + StackFrameLayout::X`), which the callee will see as its metadata:
see `callImpl` below.

### Making calls to the runtime (helpers)

Two macros (in `JitEmitter-internal.h`) wrap `Emitter::callRuntime*`; both
materialize the target address into x16 with mov/movk and `blr`:

- `EMIT_RUNTIME_CALL` → `callRuntimeWithSavedIP`: stores the current bytecode
  IP (`getBytecodeIP`, materialized from `codeBlock_->begin()` + offset) into
  `Runtime::currentIP_` before the call. Required for anything that can
  throw, allocate, or otherwise observe the IP (exception backtraces, lazy
  compilation, GC). With `emitAsserts`, the IP is invalidated after the call
  so stale-IP bugs assert in debug.
- `EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP` → `callRuntime`: preserves whatever IP
  is already saved (prologue checks, setjmp, catch dispatch, and helpers
  that neither throw nor allocate).

Historical note: the JIT once had a thunk mechanism (per-target trampolines
loading from an RO-data function-pointer table) so each helper call site
would be a short `bl`. It was disabled after costing 2.8% on a React
benchmark via branch mispredicts, and later deleted outright — see git
history for `registerThunk`/`emitThunks` if the tradeoff is ever revisited.
An x86-64 backend should not resurrect it without new measurements; the
mov-imm64 + `call reg` shape is the baseline.

### Slow paths, RO data, constants

- Slow paths are queued in `slowPaths_` (a `std::deque<SlowPath>`) during
  instruction emission and emitted after the function epilogue by
  `emitSlowPaths()`. `SlowPath` (JitEmitter.h) carries the two labels and
  `emittingIP` (so IP materialization in the slow path is correct) plus a
  type-erased lambda whose captures are stored *inline*: the constructor
  placement-news the lambda into fixed storage, static_asserting that it
  fits, is not over-aligned, and is trivially destructible. Each slow path
  therefore captures exactly the state it needs — no shared field grab-bag,
  no per-lambda heap allocation, and reading state the producer never set is
  unrepresentable. `SlowPath` is deliberately non-copyable/non-movable
  (`std::deque` never relocates elements). Typical shape: fast path branches
  to `slowPathLab` on a failed check; slow path calls the `_sh_ljs_*`
  helper, moves the result and branches back to `contLab`.
- `roData_` is a byte vector emitted after the code (`emitROData`), addressed
  pc-relative through `roDataLabel_`; it holds 64-bit constants
  (`uint64Const`, deduplicated in `fp64ConstMap_`) and the debug function
  name. The read/write/private-name property-cache base pointers are stored
  there at construction.
- Doubles/constants are loaded either inline (`isCheapConst`: ≤2 non-zero
  16-bit words → mov/movk) or from the RO-data pool (pc-relative `ldr`).

### Calls to JS (`callImpl`)

`call/callN/callWithNewTarget/callWithNewTargetLong/construct` populate the
outgoing frame metadata FRs (callee, previous-frame, saved-IP, null
saved-code-block, null SHLocals, new.target, argcount; small `CallN` forms
also copy the args), then `callImpl`:

1. Fast path: callee tag is object → GCCell kind in
   `[CodeBlockFunctionKind_first..last]` → its CodeBlock's `JITCompiled_`
   non-null → `blr` directly into the JIT'ed callee (x0=Runtime).
2. Kind out of range or not yet JIT'ed → slow path loads
   `VTable::jitCallArray[kind]` (per-CellKind dispatch, handles bound
   functions, native functions, interpreter fallback, and triggers callee
   compilation) and calls it with the same (Runtime*, Callable*) protocol.
3. Non-object callee → shared per-function `nonObjCallLabel_` slow path calls
   `_jit_throw_non_object_call` (outgoing frame is already set up, so the
   error can reference the callee value).

`CallBuiltin` instead calls the `_jit_call_builtin` C++ handler, which
finishes populating the already-arg-filled new frame and invokes the native
builtin. `CallRequire` calls `_sh_ljs_callRequire` with the module cache.

### Exceptions

If a function contains any try region, its prologue pushes one `SHJmpBuf` for
the whole function. Unwinding uses `longjmp`: a throw anywhere below performs
`_sh_longjmp` to the innermost jmpbuf. On a nonzero setjmp return, control
goes to `catchTableLabel_` (emitted by `emitCatchTable` after `leave`):

- Calls `_jit_find_catch_target(runtime, codeBlock, frame, jmpBuf,
  savedSHLocals, addressTable)`. That handler rethrows uncatchable values,
  finds the faulting bytecode IP (from `Runtime::currentIP_` if the fault was
  in this frame, else from the callee frame's SavedIP slot — the register
  stack has not been reset yet), searches the *bytecode* exception table by
  offset, restores `shLocals`/register-stack via `_sh_catch_no_pop`, and
  returns the native address of the handler BB from the embedded
  `addressTable` (4-byte label deltas emitted next to the dispatch code); if
  no handler covers the offset it pops the jmpbuf and rethrows.
- `Catch` itself (`catchInst`) then loads `runtime.thrownValue_` into the
  result FR and clears it (writes "empty").

Because the JIT does not push/pop per-frame `SHLocals`, the prologue-saved
`shLocals` value is what gets restored.

A crucial supporting invariant lives in the bytecode register allocator, not
the JIT: `RegisterAllocator::getRegClass` (`lib/BCGen/RegAlloc.cpp`) forces
*all* registers to `RegClass::Other` in any function containing a try. Since
the JIT assigns global callee-saved registers only to the Number/NonPtr
register classes, a function with exception handlers has **no global
registers at all** — every FR's canonical location is the memory frame, which
is what makes "longjmp to the entry setjmp, then re-enter a handler BB that
reads FRs from the frame" correct. Emitters of potentially-throwing
instructions still must sync live temps before the throwing path when inside
a try (`isInTry()`), because temps are per-BB and not restored by longjmp.

**Sync-only vs. sync-and-free.** The `isInTry()` emitters are not consistent
about whether the sync is followed by `freeAllFRTempExcept({})`, and the
distinction is worth understanding before porting them. The two serve
different purposes:

- The **sync** is about the *frame*: it makes the memory frame current, which
  is what a catch handler reads after a longjmp. Required whenever a throwing
  path can be taken inside a try.
- The **free** is about *registers*: temps are caller-saved, so any call
  clobbers them. Required whenever a call is emitted on a path that then
  continues in this basic block — whether the call is inline or in a slow
  path that returns to the fast path. It is *not* required when the only call
  is a non-returning throw in an out-of-line slow path, because nothing
  resumes with those temps live; the catch handler begins a new basic block,
  which re-normalizes temp state via `newBasicBlock`.

That rule accounts for all three shapes in the emitter:

| Emitter | Shape | Why |
|---|---|---|
| `throwIfThisInitialized`, `fastArrayLoad` (`#else` arm) | in-try sync, no free | only call is a non-returning throw, out of line |
| `throwInst` | in-try sync, unconditional free | emits its `_sh_throw` call inline |
| `fastArrayLoad` (`HERMESVM_COMPRESSED_POINTERS` / `HERMESVM_BOXED_DOUBLES` arm) | unconditional sync + free, no `isInTry()` | emits an unconditional `_sh_fastarray_load` call |

`throwIfEmpty` also does both; there the free is plausibly for allocation
rather than safety, since it allocates a result FR immediately afterwards
that can reuse a freed temp.

Note that `fastArrayLoad` appears in two rows: the shape depends on the
build configuration, so reading only one arm gives the opposite impression.
When porting, derive the free from "does a path that continues emit a call",
not from the nearest sibling's shape.

### Inline caches

Read caches (`GetById*`), write caches (`PutById*`), and private-name caches
use the same `SHReadPropertyCacheEntry`/`SHWritePropertyCacheEntry` arrays as
the interpreter (owned by the CodeBlock, malloc'd — stable addresses baked
into RO data). The JIT-specific twist for hidden-class identity: instead of
comparing full HC pointers (which would require materializing a 64-bit
pointer), emitted code can compare the 16-bit `HiddenClass::lazyJITId_`,
assigned on demand by `initHCLazyIDMayAlloc()`. Assigned HCs are appended to
`JITContext::Impl::usedHCs` (an `ArrayStorageSmall` rooted via
`JITContext::markRoots`), which pins them forever — required because the ID
is baked into immutable machine code. ID 0 means "unassigned"; the 16-bit
space can overflow (65535 ids), after which affected fast paths are not
emitted.

`GetByIdImpl` (a ~350-line class in `JitEmitter-property.cpp`) emits the
interpreter's
cache-hit protocol inline, in up to three tiers:

1. **Object specialization** — if at JIT time the cache entry has
   `numGoodChanges == 1` and a positive match, the entry's HC and slot are
   baked into the code: compare the object's HC `lazyJITId` against the
   16-bit immediate, load the fixed slot directly. (Valid because a
   non-dictionary HiddenClass's layout is immutable and the ID-pinning via
   `usedHCs` keeps the HC alive.)
2. **Parent specialization** — for negMatch entries (property found on the
   prototype): check the object's HC ID, load and null-check the parent,
   check the parent's HC ID, load the baked slot from the parent. When this
   tier is emitted, no generic tier follows.
3. **Generic tier** — compare the object's compressed HC pointer against the
   *live* cache entry (base pointer from RO data), load `_slot16`, split
   direct/indirect properties, decode the SmallHermesValue
   (`Emit_sh_shv_decode` shares the multi-case decode tail across load
   sites).

Miss → out-of-line call to `_sh_ljs_get_by_id_rjs`/`_sh_ljs_try_get_by_id_rjs`
with the cache-entry address. Writes have an inline tier on both backends
(`emitPutByIdInlineTier`, see "Inline write barrier" below): when
the site's write cache already names a hidden class at compile time, the
emitter bakes in a class/slot check and an inline store guarded by the
safe-store barrier predicate. A cold cache -- in particular every function
compiled under `-Xjit=force`, which compiles on the first call before any
site has run interpreted -- emits the helper call only, exactly as before.
Either way the fallback is `_jit_put_by_id`, unchanged: it implements
cache-hit, cached-add-transition (checking the RuntimeModule
`AddPropertyCacheEntry`, parent epoch and parent pointer) and falls back to
`Interpreter::putByIdSlowPath_RJS`.

### Inline allocation

`bumpAllocAndUnpoison` emits a young-gen bump allocation against the Hades
fields baked in `RuntimeOffsets` (`heap_.youngGen_.level_` /
`effectiveEnd_`), branching to a slow path on overflow; with ASan it also
calls `__asan_unpoison_memory_region`. `initGCCell` stores kindAndSize (+
debug alloc id in debug builds). Used by `newObject`,
`newObjectWithParent`, `newObjectWithBuffer` (paired with
`_jit_new_empty_object_for_buffer` / interpreter buffer handlers for the
non-fast cases), environment creation (`createEnvironment`,
`createFunctionEnvironment` inline-initialize slots to undefined), and
`alloc2InYoung` allocates object+propstorage pairs. Allocation slow paths
call the corresponding `_sh_ljs_*`/`_interpreter_*` helpers.

Since the JIT stores into freshly allocated young-gen cells before any
safepoint, those initializing stores need no write barriers (this is also why
`newObjectWithBuffer` bails to the slow path when the property count exceeds
`JSObject::maxYoungGenAllocationPropCount()`). Stores into pre-existing
objects are a mix: own-slot stores (the PutById inline tier) and
fast-array element stores (the PutByVal inline tier) ARE inlined, each
behind the "safe store" barrier predicate described below. Environment
slot stores remain `_sh_ljs_*` helper calls, which perform the Hades
barriers in C++ -- not because they cannot be inlined the same way, but
because they measured below 1.2% of cycles on the benchmarks that
motivated this work; the predicate is available if a workload ever
justifies spending the code size and complexity there.

The primary Hades-specific inline emission is that same write-barrier
predicate (`Emitter::emitSafeStoreOrSlow`, on both backends -- see "Inline
write barrier" below). The other one, present on both backends, is a
weak-root *read* barrier guard: `newObjectWithBuffer` reads the cached
`HiddenClass` from the RuntimeModule's `objectLiteralHiddenClasses_`
`WeakRoot`s, and bails to the slow path whenever
`heap_.ogMarkingBarriers_` is active.

### GC integration

The mutator can only observe emitted code at helper-call boundaries; the
frame slots (zero-initialized, always the canonical location for pointer FRs)
are the GC roots. Specifics:

- Pointer-typed FRs never live in global (callee-saved) registers across
  safepoints; before any helper call all temps are synced to frame/global.
- `JITContext::markRoots` roots only `usedHCs` (pinned hidden classes).
- Other GC pointers baked into code must be independently immortal:
  string constants are materialized via `loadConstStringInGpX` only for
  SymbolID-interned strings held alive by the IdentifierTable;
  CodeBlock/RuntimeModule/property caches/builtin table are malloc'd.
- `HERMESVALUE_VERSION == 2` static asserts appear next to every piece of
  tag-manipulation code; the tag helpers (`emit_sh_ljs_*` in
  `JitEmitter-internal.h`) encode the NaN-boxing scheme
  (48-bit data, tags in the top bits, pointer tags ≥ `HVTag_FirstPointer`,
  extended tags for undefined/null/bool/symbol/empty at bit 47).
- Compressed-pointer / boxed-double configurations are supported behind
  `HERMESVM_COMPRESSED_POINTERS` / `HERMESVM_BOXED_DOUBLES`
  (`emit_sh_cp_*`, `Emit_sh_shv_decode`); the contiguous-heap requirement in
  Config.h makes decode a simple `add xRuntime`.

### Inline write barrier

Both backends store a heap value inline without a helper call. Every such
store -- the PutById tier, the PutByVal tier, and any future consumer --
goes through one shared predicate,
`Emitter::emitSafeStoreOrSlow` (`JitEmitter.h`), which performs the store
itself only when Hades's write barrier for it is provably a no-op or a
single card-dirty; every other case jumps to the caller's helper label
without storing. The design -- the decision sequence, the first-unit
precondition, the callers' bounds, the encode's per-tag rules and its
decline case -- is architecture-independent; only instruction selection
differs, and the two backends' files are comment-for-comment parallel where
they agree. Full derivation, including the source citations for each fact
below, is in
`doc/superpowers/specs/2026-08-27-jit-inline-property-writes.md`.

**The decision sequence**, given `loc` (slot address) and `value` (the
64-bit HermesValue) in registers:

    (in the caller, ahead of every guard:)
    shv = encode(value)                                  (boxed doubles only)
    if (encoding shv would need a BoxedDouble)          → HELPER
    (in the predicate:)
    segLoc = loc & ~(kSegmentUnitSize - 1)
    if (segLoc == runtime.heap_.youngGen_.lowLim_)      → STORE, done
    if (runtime.heap_.ogMarkingBarriers_ != 0)          → HELPER
    if (compactee active)                               → HELPER
    if (word at segLoc+4 (shiftedSegmentSize) != 1)      → HELPER
    STORE shv
    if (value is a young pointer)
      card byte at segLoc + ((loc - segLoc) >> 9) = 1   (dirty)

That is: a young-generation target needs no barrier at all; a marking
collection in progress or a compaction in progress both need the full C++
barrier; and a segment whose card array is not the inline one (see below)
cannot be dirtied by this code. Otherwise the store happens, and the card
is dirtied exactly when the stored value is itself a young pointer.

**The one non-obvious precondition:** `loc` must lie within the first
`kSegmentUnitSize` bytes of its segment. The predicate derives the segment
start as `loc & ~(kSegmentUnitSize - 1)`, which is only the true segment
start under that bound -- a jumbo (multi-unit) segment is aligned to
`kSegmentUnitSize` but is several units long, so for a `loc` further in,
the mask names a later unit and the "is this a one-unit segment" test
reads object payload instead of segment metadata. This matters because the
runtime's own large-object barrier does not have this problem: it derives
the segment start from the *owning cell*
(`AlignedHeapSegment::dirtyCardForAddressInLargeObj`), while this code
derives it from `loc` alone; the two agree only while `loc` is in the same
unit as the cell head. Each caller is responsible for the bound, not the
predicate: PutById satisfies it through
`WritePropertyCacheEntry::kMaxSlot` (0xff, at most ~2KB past a cell head,
and every cell head lives in the first unit of its segment); PutByVal
satisfies it through `RuntimeOffsets::kMaxInlineStorage`
(`FixedSizeHeapSegment::maxSize()`), which bounds the indexed-storage
cell's own allocation size and so proves the cell cannot have been placed
in a jumbo segment. The predicate's own segment-size test (the `+4` check
above) is necessary but not sufficient for this -- it does not substitute
for the caller's bound.

**RuntimeOffsets maintenance contract.** The predicate bakes GC geometry
into emitted code; each fact is pinned by an `offsetof` or a
`static_assert` -- in `lib/VM/JIT/RuntimeOffsets.h` for most of them, and
at each backend's emitter use site for the `KindAndSize` size-field width
(`JitEmitter-property.cpp`) and for the immediate forms that backend's
instructions can encode (`JitEmitter-internal.cpp`: the compactee
sentinel's imm32 fit on x86-64; on arm64 the segment mask and the
inline-representability mask as logical immediates, the sentinel and the
pointer-tag bias as ADD/SUB immediates, and the two runtime offsets the
unsigned-offset LDR form reaches) -- so a GC-side change breaks the build
instead of silently miscompiling:

- `kSegmentUnitSize` is a power of two;
- `kLogCardSize == 9` (512-byte cards);
- `CardStatus::Dirty == 1`;
- the inline card array (`inlineCardsArray_`) sits at segment offset 0;
- every card index the store can produce falls at or past
  `kFirstUsedIndex`, clear of the alloc-region prefix that offset 0
  actually holds;
- `shiftedSegmentSize`'s width is the 16 bits the predicate compares;
- `KindAndSize`'s size field is at most 32 bits, so the PutByVal gate's
  plain 32-bit load at cell offset 0 contains it; where it is narrower --
  the four-byte header of a compressed-pointer build, where the kind shares
  the word -- the gate masks the kind off first;
- `kMaxInlineStorage` is at most `FixedSizeHeapSegment::maxSize()` and at
  most `GCCell::maxNormalSize()` (a cell above that bound stores 0, not
  its size, in `KindAndSize`, so the gate must reject 0 too);
- the young generation's `lowLim_`/`level_`/`effectiveEnd_` offsets, and
  the compactee's `start` offset and `kInvalidCompacteeStart` sentinel,
  including that the sentinel value fits the 32-bit immediate form the
  emitter uses to compare it;
- the width of a heap value slot (`kSmallHermesValueSize` and its log,
  which every slot index is scaled by) and, under boxed doubles, everything
  the encode bakes in: `SH_SHV_TAG_BITS` and `SH_SHV_RAW_TYPE_BITS` agreeing
  with `HermesValue32`'s own constants and with the slot width, a non-zero
  payload width (so the shift the emitted test branches on sets flags), a
  zero `CompressedHV64` tag, and one constant bias mapping all three
  HermesValue pointer tags onto the SmallHermesValue ones.

One item is deliberately *not* on that list: the `ClassFlags` bit mask
and value the PutByVal tier tests on the target's hidden class
(`indexedStorage` set, `hasIndexLikeProperties` and `frozen` clear) are
computed at runtime by writing the bitfields and reading back the union's
`_flags` member (`RuntimeOffsets::classFlagsFastArrayMask/Value`), not
`static_assert`ed, because reading a union through a member
other than the one just written is not a constant expression in C++17. An
assert at the emission site pins the values instead.

**A slot holds a SmallHermesValue, not a HermesValue,** and the two differ
in every mode that defines `HERMESVM_BOXED_DOUBLES` (both `HEAP_HV_PREFER32`
and `HEAP_HV_BOXED`). `emit_shv_encode_or_slow` (`JitEmitter-internal.cpp`)
mirrors `HermesValue32::encodeHermesValue()` case for case -- an ETag
dispatch over the inline ("compressed HV64") values, the three pointer tags
and Symbol -- with one difference, which is why it takes a label. A double
whose low bits are not zero has no inline encoding: it has to be boxed in a
heap-allocated `BoxedDouble`, and emitted code cannot allocate. That value
is DECLINED to the helper, which boxes it there.

**The encode runs first, before the tier's guards**, through the
mode-selecting wrapper `emit_shv_encode_for_slot_or_slow` (which emits
nothing at all in `HEAP_HV_64` and returns the HermesValue itself, so the
tiers need no `#ifdef`). This is a measured ordering, not a stylistic one. A
value with no inline encoding is the cheapest thing a tier can reject, and
rejecting it before the guards means such a store does not also pay for the
class compare, the cell-kind and flags tests, the index conversion and the
storage load. A scratch counter over the HV32 Release build put the decline
rate at **63.6%** on Octane Box2D (11.1M declines out of 17.4M tier entries)
against **0.01%** on pixel-grid, 0% on Richards and DeltaBlue, and 2.5% on
relay -- Box2D writes mostly doubles, and most physics doubles do not
compress. Moving the encode ahead of the guards recovered about half of the
Box2D regression the tiers had introduced (-2.5% to -1.1% against the
pre-Stage-4 build) and gained on pixel-grid as well (+2.4%).

The tiers hold the encoded value in `temp2` from there to the store; no
guard touches that register, so it costs nothing to keep. Inside the
predicate `temp2` doubles as scratch, which is sound because it is not
reused until after the store, by which point the encoded value is dead.
That invariant is free on x86-64, whose guards compare against memory and
immediates directly; on arm64 it had to be arranged, because every mask,
wide immediate and runtime word there needs a register first. Those all go
to `xScratch` (x16), the backend's non-allocated scratch, which nothing
holds a value in across an emitter call and which none of these sequences
emits a call inside of.

Everything in the predicate, the card decision included, is phrased in
terms of the original 64-bit value. The two agree on which values are
pointers -- the one pointer a SmallHermesValue has that a HermesValue does
not, a `BoxedDouble`, was already declined -- and a HermesValue carries its
pointer uncompressed, which is what the young-segment compare needs.

**Build-state gate.** The predicate and its two consumers compile in when
`HERMES_JIT_INLINE_SAFE_STORE` is set (`include/hermes/VM/JIT/Config.h`).
On both backends that is every heap-value mode under Hades. Only MallocGC
switches it off, having neither a young generation nor a card table to
reason about; there every store site
emits the unconditional helper call, exactly as before this series. No
lit feature gates emitted-code tests to a mode: `test/lit.cfg`'s `%hv-mode`
substitution names the current mode, so a test pins what is common under
one FileCheck prefix and what differs under `<PREFIX>-HV64` / `-HV32` /
`-BOXED`.

**Tests.** `test/jit/putbyid-inline.js`, `test/jit/putbyval-inline.js` and
`test/jit/inline-store-shv-shapes.js` each run the inline tiers against a
real, collecting heap and would fail -- loudly, under ASan -- if the
card-dirty store were removed; the last of those drives every
SmallHermesValue shape through both tiers, including the non-compressible
doubles that must take the helper. All three are architecture-independent
and run on both backends. `test/jit/x86-64/putbyid-inline-emitted.js` and
`putbyval-inline-emitted.js` check the emitted instructions directly, in
all three heap-value modes; `test/jit/putbyid-inline-emitted-arm64.js` and
`test/jit/putbyval-inline-emitted-arm64.js` are their arm64 counterparts,
also per mode. A caveat on
the behavioral tests: a missing card dirty only fails loudly under
`HERMES_SLOW_DEBUG`, where `HadesGC::verifyCardTable()` catches it: in a
Release build the same defect surfaces as a wrong printed value, or as
nothing at all if the reclaimed memory is not reused before the next read.
A green run of these tests on a Release tree is not, by itself, evidence
of barrier correctness -- the ASan+Debug (`HERMES_SLOW_DEBUG`) run is.

**Measured effect on HV64** (pixel-grid React benchmark, precompiled
bytecode, Release, `-Xjit`): +5.2% cumulative across the two stages that
landed first (PutById tier +4.9%, PutByVal +1.4-2.1%); pixel-grid-small
+3.3%, relay +2.5%, Octane Richards +14.7%. `_jit_put_by_id` helper calls
dropped from 810M to 67M; fast-path-eligible PutByVal helper calls from
102M to 7.7M. Environment stores (spec Stage 3) were deliberately left on
the helper path: they measured below 1.2% of cycles on these benchmarks,
so inlining them was not pursued.

**Measured effect on HV32** (`HEAP_HV_PREFER32`, Release, `-Xjit`,
interleaved A/B against the same tree with the tiers compiled out, medians
of 4-14 paired runs, encode-first ordering): pixel-grid **+6.5%**,
pixel-grid-small +1.0%, Octane Richards **+9.6%**, DeltaBlue +1.5%, relay
-0.8%, **Box2D -1.1%**.

**arm64 is unmeasured, deliberately.** The arm64 backend runs here only
under `qemu-user`, where wall-clock numbers say nothing about the hardware,
so the arm64 port was gated on correctness alone -- the jit suite on all
three arm64 trees, the `aarch64/jit-stress.js` differential in force,
threshold and assert-emitting modes, and the behavioral tests failing on
`verifyCardTable()` with the card store removed. The x86-64 numbers above
are the expectation for arm64 too: the emitted sequences are the same
decisions in the same order, and nothing in them is x86-specific beyond
instruction selection.

Box2D is the one benchmark the tiers lose on, and the reason is the decline
rate quoted above: with two thirds of its stores unencodable, the tiers can
only add work there. Moving the encode ahead of the guards halved that loss
(it was -2.5% with the encode inside the predicate) but did not remove it;
what remains is the encode's own handful of instructions on every store,
plus the tier prologue on the stores that fail a guard rather than the
encode. Closing the rest would need a per-site record of how often a site
declined, so that a site which always declines can stop emitting -- or
stop executing -- the tier at all. That is a follow-up, not part of this
series.

### Recompilation

A compiled function can be compiled again later and the new body
installed for future calls. Full rationale and the swap protocol are in
`doc/superpowers/specs/2026-09-04-jit-recompilation-design.md`; the
per-version feedback records described below are a later retrofit, in
`doc/superpowers/specs/2026-09-07-jit-version-data-design.md`. This is
the summary.

**Records.** Each compiled body has its own `JitVersionData`: a
back-pointer to the `CodeBlock`, the body's entry point, and that
body's own observations -- `declineCount`, the cold-site lists, and
`consumerRecords` for future per-site feedback. `JitFunctionData` keeps
only the control state that spans versions: `recompileBudget`,
`version`, the `current` record, and the `retired` list of previous
ones. Splitting it this way means one version's feedback never blends
into another's: a kind that reaches version 1's helper may be handled
inline by version 2 and never reach its helper at all.

**Trigger.** `_jit_put_by_id` is the only ById write helper that
carries a version record, so it is the only trigger site in v1: every
call into it is a decline of the inline PutById tier, and it bumps the
CALLING body's own `declineCount` -- the helper receives the calling
body's `JitVersionData` directly, and reaches the `CodeBlock` through
its back-pointer. At that body's own decline threshold -- snapshotted
into its `JitVersionData` when it was compiled, default 64, set by
`-Xjit-recompile-threshold` -- it hands off to
`JITContext::considerRecompile`, which resets the
counter and gates in order. First, staleness: if the record is not the
function's `current` version, the call returns immediately -- a
retired body's declines are counted (they still bump its own frozen
counter) but never spend budget, so its events are inert. Then the
existing checks on the current version: budget left, `coldByIdSites >
0` (the emitter's count, from that version's compile, of Get/PutById
sites that declined tier emission because their cache was cold), and
at least one of those specific caches now names a class. If that check
fails -- a polymorphic function whose tiers are already emitted -- the
counter resets and the budget is kept; declines from GetById alone do
not trigger, since the shared SH GetById helper has no version record
to carry the counter.

**Install.** A passing check recompiles synchronously, right there --
compilation never runs JS, so there is no reentrancy. The candidate
record is filled first (cold-site lists moved from the emitter, entry
point set), the function's current record, if any, moves onto
`retired`, the candidate becomes the new `current`, and only then does
`setJITCompiled()` publish the new body -- a single pointer store into
the `CodeBlock`; every call already loads that pointer per invocation
(JIT call sites and the interpreter alike), so there is no code
patching and no call-site tracking. Publication comes last so
`current` never describes a body calls cannot yet enter. Activations
already running the old body finish on it: the old body stays valid
and fully guarded, and its record stays on the `retired` list rather
than being freed. Retired records, and the bodies they describe, are
never freed in v1 -- reclaiming them needs a safepoint scan of the
native stack(s) for return addresses, deferred as dz issue
`01a06d06-a7d7`. That isolation has no exceptions: a compile writes no
version-dependent RuntimeModule state -- no code address it emits ever
reaches the module, string switches included. The module's shared
switch tables hold only case indices, and each body resolves an index
through a jump table of its own (see `stringSwitchImm`).

**Budget.** `-Xjit-max-recompiles=N` sets the per-function recompile
budget; default 2, and `0` restores the exact compile-once behavior
this mechanism extends. `-Xjit-recompile-threshold=N` sets how many
declines of one compiled body precede a recompile check; default 64,
and `0` is clamped to 1 (there is no "off" value -- that is what
`-Xjit-max-recompiles=0` is for). It applies to bodies compiled after
it is set, which in practice means all of them.

**Dump labels.** `-Xdump-jitcode` prints a recompile's banner and
success lines with a `(version N)` suffix for N >= 2 (a first compile
prints no suffix); perf jitdump symbol names get a matching ` vN`
suffix so profiles do not conflate versions.

**Non-goals.** No deopt and no speculation: every version remains
helper-guarded and all versions are semantically interchangeable, so
nothing is ever invalidated. No OSR: a hot loop inside a long-running
activation keeps running its old body until it returns. No async or
background compilation: recompiles are synchronous, like today's
threshold compiles.

Both backends pass each body's version record to its helpers, but
arm64 stays dormant: its `Emitter` never appends to the cold-site
lists, so a freshly compiled body's `coldByIdSites` is always zero and
`considerRecompile`'s gate never lets a recompile fire. The tree builds
and the jit suite passes there; live recompilation arrives with the
arm64 port.

**ByVal shape records.** The Records paragraph above named `consumerRecords`
for future per-site feedback; it now holds a `JitConsumerRecords`, a deque of
per-site `JitByValSiteRecord`s keyed by the PutByVal instruction's bytecode
offset -- a deque, not a vector, because emitted code embeds the address of an
entry's `helper` slot, so entries must never relocate.

Tier selection is monotone: tiers are only ever added, never dropped (spec:
`doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md`,
"Monotone emission"). The JSArray fast-array tier is a static prior, not a
decision -- it is emitted at every site `HERMES_JIT_INLINE_SAFE_STORE` permits
one, in every version, exactly the pre-feedback behavior; nothing observes its
hits, so no evidence about it is ever consulted and it is never instrumented.
The typed-array tier is emitted at a site iff the record holds an observed
supported kind (`taKind != kTAKindNone`): since `taKind` never un-learns,
selecting from that one field is already monotone, and no previously-emitted
set needs consulting. A site that hosts both keeps the shipped chaining
(typed-array tier first -- it is what the site was observed declining on --
kind miss chains into the JSArray tier rather than the helper, then
`targetKnownObject`).

Poison preserves the first kind: `kTAKindPoly` does not exist. The record
keeps `taKind` = the FIRST observed supported kind permanently, plus
`taPoisoned`, set when a DIFFERENT supported kind is later observed (the first
kind is kept, not replaced). Selection and the progress check both ignore
`taPoisoned` -- a poisoned site still gets (or keeps) its first kind's tier, so
that half of its traffic runs inline while the other half declines toward
demotion; a second typed-array tier per site (PIC-style growth) is out of
scope. Recording, done by the helpers below, is otherwise as before:
`jsArraySeen` for a JSArray target, `otherSeen` for an unsupported kind, any
other object, or a non-object.

The recording helpers `_jit_put_by_val_{loose,strict}` -- the x86-64 PutByVal
emitters' helper-path targets, standing in front of the unchanged
`_sh_ljs_put_by_val_{loose,strict}_rjs` -- classify every declining target,
update the record (setting `changed` on any field's first transition), and
join the same decline/threshold/`considerRecompile` tail `_jit_put_by_id`
uses, so the two sources share one counter, one threshold, and one budget.
Unlike the PutById tiers, the call to a ByVal site's current helper is
indirect: `JitByValSiteRecord` holds a mutable `void *helper` slot
(initialized, if still null after carry-forward, to
`_jit_put_by_val_{strict,loose}` per the instruction), and the emitter emits
`movabs xScratch, &site.helper; call qword ptr [xScratch]` -- one load more
than a direct call, on a path that is slow by definition. The six argument
registers are set up as for any helper call; a four-argument plain callee
ignores r8/r9 under SysV, so one call sequence serves both the recording and
the plain helper.

`considerRecompile`'s progress check reads, per site, whether the observed
`taKind` outruns what the current body emitted there
(`isJitSupportedTypedArrayStoreKind(taKind) && taKind != specializedTAKind`;
`specializedTAKind` is what THIS body emitted, reset in carry-forward, filled
at emission) -- the only ByVal progress source under monotone emission, since
the JSArray tier can never be missing where evidence would want it. A
triggered recompile re-reads the carried-forward record per site exactly as
described above. The typed-array tier itself guards, in order: the target is
an object and its cell-kind byte matches the specialized kind (both ahead of
any value-based decline, so a duo site's kind miss chains into the JSArray
tier rather than declining to the helper on a non-number value the JSArray
tier could still handle), the fast-array class-flags mask (the same
fast-index-properties/`frozen` compare of the hidden class's flags the JSArray
tier uses -- needed because
an out-of-range `defineProperty` followed by `freeze` must still throw on a
strict in-bounds store), the stored value is a number the specialized kind can
hold, the key is a uint32 (the fast-array tier's own sequence), the index is
in bounds, and the buffer is attached (a free check riding the `data_` load
the store needs anyway); every guard decline falls through to the current
helper, preserving whatever it does for that case -- strict-mode throws
included. Because typed-array storage holds no GC pointers the tier needs no
write barrier, and its only heap-encoding-sensitive steps are the
compressed-pointer decodes it shares with the fast-array tier, so it is
heap-mode-neutral across HV64, HV32, and BOXED. It is also GC-kind-neutral in
the other direction: the JSArray tier exists only where
`HERMES_JIT_INLINE_SAFE_STORE` is nonzero (MallocGC disables it), but the
typed-array tier needs no write barrier and stays available unconditionally on
x86-64, which is why `classFlagsFastArrayMask()`/`Value()` moved out of
RuntimeOffsets.h's Hades-only section.

**Slow-path demotion.** A site's demotion state lives entirely in its helper
slot: while it points at a recording helper the site is still being watched;
once flipped to the matching plain helper (`_jit_put_by_val_strict` ->
`_sh_ljs_put_by_val_strict_rjs`, loose likewise -- strictness read from the
slot's current value, no extra field) every subsequent store calls the
pre-feature helper directly, with no recording, no counting, and no extra hop.
Terminal is terminal: nothing ever flips a slot back, and carry-forward copies
a demoted slot's value into the candidate record, so demotion survives
recompiles triggered by other sites in the same function. `considerRecompile`
runs a demotion pass over every still-recording site on each threshold
crossing, ahead of the budget/`enabled_` early-outs (demotion needs no budget
and must run at budget 0): a never-observed site is left alone (flipping it
would silence a site that might still start learning); a site that can still
progress -- the term above, evaluated against the CURRENT body -- resets its
stability counter, but only while a recompile could actually happen (budget
nonzero and the JIT still `enabled_`; once either fails -- the budget is
exhausted or the JIT has been disabled -- no recompile can ever happen again,
so an observed site can never be rescued and is demotion-eligible regardless
of `taKind`); a site
whose record changed since the last crossing (`changed`, set by the recording
helper on any field's first transition -- same-shape declines set nothing)
also resets; otherwise its `unchangedCrossings` counter increments, and at
`kDemotionStableCrossings` (3) the slot flips and `NumByValDemotions` counts
it. `changed` is cleared for every site at the end of the pass. A fresh site's
first crossing is always a `changed` crossing, so demotion of a site observed
from cold lands at the fourth crossing (one changed + 3 stable). The version
record keeps a `uint32_t recordingByValSites` count, decremented on each flip,
so the pass is a constant-time skip once every site is demoted -- declines
from other sources (ById, other ByVal sites) keep driving crossings after
that. Retirement demotes too: the install step, moving `current` into
`retired`, flips every still-recording site in the retired record without
counting the flips (retirement IS that body's terminal state; its
observations influence nothing by construction, so recording past that point
is pure waste no demotion pass would otherwise catch, since the retired
body's own crossings die at the staleness gate first).

The compile banner gains a line, in the cold-ById line's style, whenever any
site was observed: `JIT ByVal sites: N observed, M specialized` (under
monotone emission, "specialized" counts sites with an emitted typed-array
tier only -- the JSArray tier's presence is unconditional and not part of this
count); `-Xdump-jitcode=3` additionally prints an `// Inline typed array store
(kind K)` comment at each specialized site, so both counts and kind selection
are pinnable in lit tests. `-Xjit-max-recompiles` defaults to 2 (was 1):
demotion sharpens budget exhaustion's consequence from "keeps recording
uselessly" to "recording ends and the site is frozen permanently", and two
recompiles cover the common case of one improvement per consumer family
(ById warmth, ByVal specialization) landing at different times. Full design:
`doc/superpowers/specs/2026-09-08-jit-putbyval-typed-array-design.md`
(tier mechanics) and
`doc/superpowers/specs/2026-09-09-jit-putbyval-monotone-demotion-design.md`
(monotone emission, poison, demotion -- supersedes the first document's
sticky-flag instrumentation and drop-tier rules where they disagree).

Tests: `test/jit/x86-64/recompile-byid-warm.js` (headline: the tier is
absent at version 1, present at version 2, and absent entirely under
`-Xjit-max-recompiles=0`), `recompile-cold-sites.js`,
`recompile-mid-recursion.js` (a retired body still on the native stack
during its own replacement), `recompile-deterministic.js` (two runs
of the headline test produce identical dumps, modulo ASLR-sensitive
hex), `recompile-staleness-budget.js` (a two-phase pin: a retired
body's declines during a deep unwind must not spend the current
version's budget, and the current version must still trigger
afterward, proving the gate suppresses the stale declines specifically
rather than disabling recompilation), and
`recompile-threshold-flag.js` (the same program recompiles at
`-Xjit-recompile-threshold=8` and does not at the default 64).

**GetByVal load tiers.** GetByVal joins the same `byValSites` deque, keyed by
its own bytecode offset (siteId is the offset, so a get site and a put site at
different offsets can never collide), and reuses every mechanism above
verbatim: `JitByValSiteRecord`, the monotone tier rule, poison-keeps-first-kind,
the indirect `helper` slot and its `movabs`/`call [slot]` sequence, the shared
decline counter/threshold/budget, and the demotion pass (`isRecordingHelper`
additionally recognizes `_jit_get_by_val`; `demoteSite` maps it to
`_sh_ljs_get_by_val_rjs`, alongside the put slots' strictness-derived mapping).
Full design: `doc/superpowers/specs/2026-09-11-jit-getbyval-design.md`.

The one real difference from the store side is unconditional emission and a
second kind predicate:

- The **JSArray load tier has no `HERMES_JIT_INLINE_SAFE_STORE` gate at all**:
  a load takes no write barrier, so `emitGetByValFastArrayTier()` is emitted
  at every site, in every version, under every GC -- MallocGC included. This
  is the one place the get and put sides genuinely diverge: the put fast-array
  tier is unavailable under MallocGC, but the get one is not, which is why the
  jit suite now runs (and must be green) on the MallocGC tree, not just build
  there. Guards mirror the put tier's (exact `JSArrayKind`, uint32 key,
  `ArrayImpl::at()`'s range check, an empty slot) with two load-specific
  outcomes: an out-of-range index still DECLINES to the helper rather than
  answering `undefined` (the prototype chain may hold an indexed property
  there), and there is no `fastIndexProperties`/frozen check at all --
  `tryFastGetComputedNoAlloc()`'s own comment explains why the empty-slot
  check already subsumes it on the read side. Unboxing the loaded
  SmallHermesValue is total (HV64: identity; HV32/BOXED: the same
  small-int/compressed-pointer/boxed-double decode a property read uses) --
  unlike a store, a load's encode step never has anything left to decline.
- **Kind support differs from the store side, so there are two predicates.**
  `isJitSupportedTypedArrayStoreKind` (unchanged) excludes Uint8Clamped
  because clamping is a store-side conversion; `isJitSupportedTypedArrayLoadKind`
  (JitFunctionData.h) is that predicate plus Uint8Clamped, since a clamped
  array's element reads exactly like a plain Uint8 one. Both sides still
  exclude Float16 (no F16C dependency) and the BigInt kinds (a load would need
  to allocate a BigIntPrimitive, which no inline tier does). Each recording
  helper passes ITS OWN predicate into `recordByValObservation` -- put helpers
  the store predicate, `_jit_get_by_val` the load predicate -- so a site's
  `taKind` is only ever set after validation against the operation that
  observed it. The two ByVal-progress predicates in JitCompiler.cpp (the
  recompile-progress check and the demotion actionable-progress check) test
  the UNION of both predicates rather than the store one alone: a Uint8Clamped
  kind can only ever have been recorded by a get site (a put site's own
  recorder would reject it as "other" before it ever reached `taKind`), and
  each emitter still independently re-validates its own predicate before
  specializing, so the union only widens progress/demotion bookkeeping, never
  which tier gets emitted.
- **Typed-array loads read out-of-bounds and detached buffers as `undefined`,
  not a decline.** `emitGetByValTypedArrayTier()` branches a bounds failure or
  a null `data_` straight to an inline `mov undefined`, with no helper call
  and nothing recorded -- unlike every other guard in either tier, which
  declines. There is nothing to learn there: the source's shape already IS
  the specialized kind, only the index or attachment differs. Element
  conversion mirrors the interpreter's typed paths (signed/zero extension
  then `cvtsi2sd` for the integer kinds, `cvtss2sd` for Float32, a direct load
  for Float64) with one correctness step float kinds alone need: NaN
  canonicalization. A raw f32/f64 element can hold any bit pattern, including
  ones that alias this engine's NaN-boxed non-number tag space, so the tier
  self-compares the loaded double and replaces any NaN with the canonical
  quiet NaN before encoding (integer kinds always convert to a finite double
  and skip this). At a site with both tiers, the typed-array tier runs first
  (kind-checked before anything else) and its kind miss chains into the
  JSArray tier rather than the helper -- the same duo chaining the store side
  uses, verified by `recompile-getval-duo.js`.

Tests (the get twins of the put set, plus the semantics files new to this
feature): `test/jit/x86-64/recompile-getval-{trigger,mono-jsarray,duo,
poisoned-int32-first,poisoned-float64-first,demote,demote-delay,
demote-carried,demote-retired,demote-budget0}.js`,
`getbyval-inline-emitted.js` (pins the indirect recording call, the JSArray
tier's empty-check decline, and a specialized TA tier's load+convert),
`getval-conversions-emitted.js` (the dumped instructions for each supported
kind), and the arch-neutral `test/jit/getval-conversions.js` (bit-pattern
round-trips per kind, including the NaN-canonicalization pin), `getval-guards.js`
(hole/prototype/accessor semantics, out-of-bounds and detached-buffer
`undefined` reads, non-uint32 keys, Arguments decline) and `getbyval-inline.js`
-- all diffed against the interpreter, and exercised on arm64's helper path
too.

### Arithmetic, comparisons and NaN handling

The binary/unary arithmetic and comparison emitters share a template
(`arithBinOp`, `arithUnop`, `compareImpl`, `jCond`, `bitBinOp` +
DECL_* macro tables in JitEmitter.h) with per-op fast-path callbacks:

- Non-number detection exploits NaN-boxing: every boxed (non-double)
  encoding is an unsigned value above `HVTag_First << 48`, i.e. a NaN bit
  pattern, so `fcmp x, x` + `b.ne`, or an `fcmp` of both operands + `b.vs`
  (unordered), routes both boxed values *and* real NaNs to the slow path.
  Real-NaN operands therefore take the slow C call and are handled there —
  slower but correct.
- The `*N` bytecode forms (`AddN`, `JLessN`...) set `forceNumber`: operands
  are statically known numbers, checks and slow paths are omitted entirely.
- Comparison condition codes are chosen NaN-aware. On arm64, unordered
  `fcmp` sets N=0,Z=0,C=1,V=1, so: `<`→kMI, `<=`→kLS, `>`→kGT, `>=`→kGE all
  evaluate false on unordered operands (JS semantics), while their negations
  (used by the inverted `JNot*` branch forms) evaluate true. Branch layout
  depends on this: the non-inverted conditional branch may be emitted before
  the `b.vs` slow-path check (safe: false on unordered), the inverted one
  must come after it. This table must be re-derived for x86-64
  (`ucomisd` sets CF/ZF/PF; unordered sets PF and the classic idiom is
  above/below conditions + explicit `jp`).
- Bit ops (`bitBinOp`) prove both operands are exact int64 doubles
  (`fcvtzs` + `sbfx #0,#63` sign-trick + round-trip `fcmp`), operate on the
  low 32 bits (which is exactly ToInt32 for exact ints; shifts use the
  hardware modulo-32 semantics matching the spec's `& 31`), and re-encode
  via `scvtf`/`ucvtf` (unsigned only for `>>>`).
- `Math.imul` compiles to a dedicated `Imul` bytecode instruction (lowered
  from `CallBuiltin(Math.imul)` by the compiler, not emitted by IRGen) and
  joins the `bitBinOp` tables on both backends (`DECL_BIT_BINOP` in
  JitEmitter.h on x86-64, the arm64 equivalent), reusing each backend's
  existing int32-proof machinery unchanged: `imul r32, r32` on x86-64,
  `mul w, w, w` on arm64, both on the low 32 bits the guard already proves
  is ToInt32. The two backends' int32 proofs accept different ranges
  beyond exact int32 (x86-64 round-trips through `cvttsd2si`, arm64 through
  `fcvtzs` + the 63-bit sign-extension trick), but that asymmetry does not
  matter here: the multiply only consumes the low 32 bits of each operand,
  which is ToInt32 under either guard, and the shared slow path (BigInt,
  Symbol, non-numeric operands) falls back to `_sh_ljs_imul_rjs`. This
  replaces a `CallBuiltin` -- frame construction, builtin-table dispatch,
  and argument marshalling -- with one multiply instruction on both
  backends, and in the interpreter and shermes as well.
- `strictEqualImpl` is a three-tier pipeline: raw 64-bit compare when a side
  is statically Bool/OtherNonPtr; `fcmp` alone when a side is statically
  Number; otherwise `fcmp`, and on unordered a tag-dispatch chain (double
  check → tag compare → object identity / string length-and-flags fast
  reject → deep compare via `_sh_ljs_strict_equal`, which takes operands by
  value and never allocates).
- `%` marshals both doubles into d0/d1 and calls `_sh_mod_double` — the same
  helper the native backend uses, so fmod corner cases match by
  construction.

### Encoding-limit handling

Immediate/displacement limits are handled by choosing the sequence up front:
helpers like `emit_load_from_base_offset` (and, since the 2026-08 fixes,
`_loadFrame`/`_storeFrame` and the cache-entry address arithmetic) test the
value against the encoding — `a64::Utils::isAddSubImm` and friends — and
pick a multi-instruction fallback when it does not fit. The older
optimistic-emission idiom, `EXPECT_ERROR(...)` (emit the short form, let the
custom asmjit error handler swallow the one expected error code, fall back),
survives at three sites (`loadConstStringInGpX`, `loadParam`, and
`storeVal` in the object-literal visitor); prefer the up-front check in new
code — it is what the fallback-fix series converged on, and it is easier to
port since the predicate is explicit rather than encoded in an error code.
Unexpected asmjit errors still longjmp out of the compilation and mark the
function don't-JIT.

### Debugging aids

- `-Xdump-jitcode=` bit flags (`DumpJitCode`): code listing (via the asmjit
  logger), compile status, per-instruction error, `brk` insertion,
  entry/exit tracing (`_sh_print_function_entry_exit`).
- `emitAsserts` (`-Xjit-emit-asserts`): IP invalidation after calls, plus
  `assertPostInstructionInvariants` at compile time (every temp accounted
  for, no dirty FRs).
- Counters (`JIT_COUNTERS`: NumCall, NumCallSlow) incremented inline when
  enabled.
- Linux `perf` jitdump + source-comment sidecar via `PerfJitDump`.
- `utils/jit/jit-dump.sh` / `jit-diff.sh`: capture a canonicalized dump of
  all JIT-emitted code from a `hermes` binary and diff two binaries'
  output, separating comment-only changes from instruction changes. The
  tool of choice for verifying that a refactor did not change emitted
  code — and for A/B-ing arm64 against a future x86-64 backend at the
  "same fast/slow path decisions" level.

### Working on the JIT without arm64 hardware

`aarch64/README.md` describes how to cross-compile for aarch64 Linux and
run the result under `qemu-user`, which compiles *and executes* JIT'ed code
on an x86-64 host: a host `hermesc`/`shermes` build feeds
`IMPORT_HOST_COMPILERS`, and `QEMU_RUN_PREFIX` (already wired into lit)
makes `check-hermes` run target binaries under emulation. `test/jit` passes
there. Two traps worth knowing even if you never use the setup:
`HERMESVM_ALLOW_JIT` must be **2**, not 1 — `=1` builds a working JIT but
leaves the lit feature unset, so the whole JIT suite silently skips while
reporting success; and ASan cannot be combined with qemu-user, so
emulation catches logic divergence but not the memory bugs an ASan build on
real hardware would. arm64 macOS remains a supported (and much faster)
JIT host — see the Config.h row in the source layout table.

arm64 has the same three-tree heap-value-mode matrix x86-64 does (see "The
heap-value-mode build matrix" below), and any change to slot widths or to
the inline write barrier must be built and run in all three. Configure
`cmake-build-arm64-hv32` and `cmake-build-arm64-boxed` exactly like
`cmake-build-arm64` -- same toolchain file, `HERMES_UNICODE_LITE`,
`IMPORT_HOST_COMPILERS` and `QEMU_RUN_PREFIX` -- plus
`-DHERMESVM_HEAP_HV_MODE=HEAP_HV_PREFER32` and `=HEAP_HV_BOXED`
respectively. `aarch64/qemu-sanity.sh` takes a build directory, so it runs
against any of the three.

## The x86-64 backend

**Milestone 5 status: the backend reaches arm64's full opcode surface.**
`JitEmitter-stubs.cpp` -- the file that once held every emitter that
declined outright -- is gone; there is no x86-64-specific "unsupported
opcode" list left to enumerate. The one permanent decline is
`AsyncBreakCheck`, and it is not an x86-64 gap at all: `EMIT_UNIMPLEMENTED
(AsyncBreakCheck)` lives in the architecture-independent driver
(`JitCompiler.cpp`) and declines identically on both backends. Everything
else -- exceptions/try/catch/finally with the per-function catch table,
switches (dense uint jump table and string), iterators/for-of, arguments
(reified and not), for-in, strings (AddS, AddEmptyString, LoadConstString,
LoadConstBigInt), typeof/typeOfIs/jmpBuiltinIs, direct eval -- compiles,
in all three heap-value modes.

The headline gate is `test/jit/x86-64/stress.js`, a standing lit copy of
`aarch64/jit-stress.js` (the differential soak test the plan requires):
one program that touches arithmetic, strings, property ICs with shape
transitions, object/array literals, closures/recursion, exceptions across
frames, switches, classes/super/getters, generators, destructuring,
for-of/for-in, `arguments`, higher-order builtin callbacks and a JSON
round-trip. Under `-Xjit=force -Xjit-crash-on-error`, at both -O and -O0,
with and without `-Xjit-emit-type-asserts`, on HV64, HV32 and BOXED alike,
every function in it compiles and the output is byte-identical to the
interpreter -- zero declines, ASan clean. That property, not any single
opcode's test, is what "opcode-complete" means for this backend now.

**Fixed: destination-FR exclusion from pre-call syncs in try regions.**
Milestone 6 confirmed that excluding an instruction's own destination FR
from the pre-call `syncAllFRTempExcept` is unsound inside a try: register
allocation coalesces a live variable's phi with that destination, so the
prior value was dropped instead of stored, and the handler read the frame
slot's `_sh_enter` zero fill --
`var y = "prior"; try { y = o.throwingGetter; } catch { use(y); }` yielded
`0`. Identical on arm64 and x86-64, on HV64/HV32/BOXED, at `-O` only.
Plain calls were unaffected (`CallInst` syncs the whole frame), and the
JIT asserts could not catch it: the value is well-formed, merely stale.

The exclusion spanned 54 sites per backend, 108 across both (39 of the
guarded `frRes != x ? frRes : FR()` form plus 15 unconditional
`syncAllFRTempExcept(frRes)`, the latter having no aliasing guard at all),
so the fix is central rather than per-site: `syncAllFRTempExcept` itself
now drops the exclusion when `isInTry()`, in both backends. That is sound
because syncing an extra FR only stores that FR's current value -- the
exclusion was always a pure optimization, and every call site issues it
before allocating its destination register, so the destination still holds
its pre-instruction value at sync time. The cost is one extra store per
throwing instruction, and only in functions that contain a try; those have
no global registers anyway (`RegisterAllocator::getRegClass` forces
`RegClass::Other` there), so the store goes straight to the memory frame.
`test/jit/try-catch-dest-reg.js` is the minimal repro and is now a plain
passing regression test on both architectures; its header carries the
before/after dump evidence. This is the first deliberate divergence of
arm64 emission from the export base: the port's byte-identical constraint
covered the port itself, not a subsequent cross-backend bug fix.

The 497-file differential sweep over `test/hermes/*.js` (plain `hermes`
vs `-Xjit=force`, same binary) went from ~270 files compiling at least one
function in milestone 4 to 480 in milestone 5, run on all three
heap-value modes (HV64: 480; HV32: 479, one file crossed the sweep's
10-second interpreter timeout under this mode's extra decode overhead;
BOXED: 480). Of 497 files, the same 6 differ between the interpreter and
the JIT run in every mode, and all 6 are explained and not JIT bugs: 3
are nondeterministic even between two plain interpreter runs with no JIT
involved at all (random array-literal contents, a wall-clock timestamp,
a property-limit count that depends on incidental memory layout), and 3
are deliberate stack-exhaustion tests (`stack-overflow.js` and friends)
where the JIT recurses measurably deeper before hitting the
native-stack-overflow check than the interpreter does, because a
compiled JS frame uses less native stack per call than an interpreted
one -- a stable capacity difference between the two execution modes,
verified by running each side several times and confirming it lands on
the same value (or, for one file, oscillates between two ASLR-adjacent
values) every time, never anywhere near the other side's number. Five
further files time out under the sweep's plain invocation in most modes;
all five have their own lit `RUN:` line with flags the sweep does not
pass (`-time-limit=`, `-gc-max-heap=`, `-test262`, `-lazy`) and are
unrelated to JIT correctness.

Milestone 4 added object and array literals via inline heap buffers, the
three-tier property inline-cache architecture with HiddenClass lazy-ID
pinning (object specialization, parent specialization, generic tier),
globals, and the fast-array family under -typed, in all three heap-value
modes (HV64, HV32, BOXED; see the build matrix below).

Milestone 3 adds arithmetic, comparisons, branches, bit operations, type
assertions, young-gen bump allocation, environments, closures, calls
(Call/Call1-4, Construct, CallWithNewTarget, CallBuiltin, CallRequire)
with JIT-to-JIT fast path, class and generator initialization. Opcode
coverage: LoadParam/LoadConst*/Mov/Ret, Add/Sub/Mul/Div/Mod (N variants),
Inc/Dec/Negate, ToNumber/ToNumeric, all comparisons/branches, BitAnd/Or/
Xor/shifts/BitNot, ToInt32/ToUint32, type asserts, environment and
function field access.

**Status: the x86-64 port is complete, per the spec's v1 definition**
(`doc/superpowers/specs/2026-08-23-x86-64-jit-design.md`, "Scope and
goals": full `test/jit` plus the differential stress matrix passing
natively under ASan, in both heap modes, with perf "sane"). All 6
bring-up milestones are done:

- `test/jit/*.js` (the arm64-authored suite) is no longer gated to
  arm64: `test/jit/lit.local.cfg` skips the directory only when no
  `jit-arch-*` feature exists at all, and the audit that came with the
  un-gating found no file needing an architecture gate. Its 50 files
  (one still `!slow_debug`-gated; the former `XFAIL` repro above now
  passes outright) run green on x86-64 alongside the 23
  x86-64-specific tests under
  `test/jit/x86-64/` -- natively, under ASan+Debug, in all three
  heap-value modes (HV64, HV32, BOXED), and again on the Release build
  with asserts compiled out. The Release run's unsupported file is a
  *different* one, though: `slow_debug`/`debug_options` flip with
  build type, so `large_literal_obj.js` runs there and
  `getbyid-fast.js` (`REQUIRES: debug_options`) is skipped instead --
  same 72/1 count, not the same membership (see `doc/JITTesting.md`,
  "Release-build validation").

  The counts above predate the 2026-08-27 inline-write-barrier series
  (`doc/superpowers/specs/2026-08-27-jit-inline-property-writes.md`),
  which added seven files. Its three behavioral tests are
  architecture-independent and all live in `test/jit/`, where both
  backends run them; only the two x86-64 `-emitted` pin tests stayed
  under `test/jit/x86-64/`, and arm64 has two of its own alongside the
  behavioral ones. Those arm64 pin tests are why the x86-64 run of
  `test/jit` now reports three unsupported files rather than one:
  - `test/jit/putbyid-inline.js` -- the PutById inline tier against a
    real, collecting heap; proves a missing card dirty is a live bug, not
    a theoretical one. Run on every backend that has the tier.
  - `putbyid-inline-emitted.js` -- pins the instructions the PutById
    tier emits, per mode via `SPEC-%hv-mode` prefixes. Both `-emitted`
    tests are `UNSUPPORTED: handle_san`: under `HERMESVM_SANITIZE_HANDLES`
    the runtime boxes every number, so the encoder declines the
    inline-number case outright and the pinned encode sequence does not
    exist (a deliberate emission difference, not a regression).
  - `test/jit/putbyval-inline.js` -- the PutByVal fast-array inline store
    against a real, collecting heap, same card-dirty hazard as above, plus
    the jumbo-storage array that proves the `kMaxInlineStorage` gate is
    load-bearing. Run on every backend that has the tier.
  - `putbyval-inline-emitted.js` -- pins the instructions the PutByVal
    tier emits, per mode via `SPEC-%hv-mode` prefixes.
  - `test/jit/inline-store-shv-shapes.js` -- drives every
    SmallHermesValue shape through both tiers into old objects across
    young-gen collections, including the doubles that have no inline
    encoding and must take the helper and be boxed. Run on every backend
    that has the tiers.
  - `test/jit/putbyid-inline-emitted-arm64.js` and
    `test/jit/putbyval-inline-emitted-arm64.js` -- the arm64 counterparts
    of the two `-emitted` tests, `REQUIRES: jit-arch-arm64` and so
    unsupported on the x86-64 trees (and `UNSUPPORTED: handle_san` for
    the same reason as the others).
- The `aarch64/jit-stress.js` differential matrix -- interpreter vs.
  `-Xjit=force`, with and without `-Xjit-emit-type-asserts`, at both
  `-O` and `-O0` -- is byte-identical on every config exercised,
  including the Release tree (the first NDEBUG x86-64 run in this
  port; no assert-side-effect-dependent divergence found).
- A first perf sanity pass on the Release build clears the spec's "must
  not lose to the interpreter on hot loops" bar on all three sanity
  workloads: 2.25x (numeric loop), 1.61x (scaled `stress.js` shapes),
  1.45x (property/IC-heavy loop). No tuning attempted; that is
  post-v1 work per the spec.
- arm64 stayed byte-identical throughout every milestone-6 change: the
  self-compare and stored-baseline-diff gates were re-run after each
  x86-64-only commit and after the final corpus growth (see "x86-64
  dump baseline" below), and every diff traces to test-corpus additions,
  never to a changed instruction in an existing function.
- x86-64 has its own dump-baseline and `jit-diff.sh`-based regression
  workflow (below), a documented five-configuration CI-shaped matrix
  with the gates each one carries, and the perf sanity pass above.
  Counter support (`-Xjit-emit-counters`, NumCall/NumCallSlow) has
  worked since it landed and needed no milestone-6 changes. That matrix
  has since grown the two arm64 heap-mode configs, to seven.

Full commands and gate-by-gate evidence are in `doc/JITTesting.md` (see
the pointer paragraph at the end of this section); the CI-recipes
paragraph there also documents the release-build/perf run in detail.

**Findings from the port.** All three were originally left to the
maintainer, since each is cross-backend or arm64-only and so touches
arm64's byte-identical contract; all three have since been fixed on this
branch. They are recorded in full, with reproductions, in
`doc/superpowers/specs/2026-08-23-x86-64-jit-design.md`'s "Delivered"
section:

1. **Try-region destination-sync bug** (fixed): the pre-call
   `syncAllFRTempExcept` excluded the instruction's own destination FR,
   which is unsound inside a try -- see the paragraph above. Fixed
   2026-08-26 centrally in both backends' `syncAllFRTempExcept`, which
   now ignores the exclusion when `isInTry()`. Regression test
   `test/jit/try-catch-dest-reg.js` (previously `XFAIL`) passes outright
   on both architectures. Unlike finding 2, this one does change emitted
   code: `jit-diff.sh` reports `INSTRUCTIONS CHANGED` on both stored
   baselines, in each case a single added store of the destination FR
   before the throwing call in the repro's try region (arm64 folds it
   into the neighbouring `stp`). That divergence from the export base is
   deliberate.
2. **arm64 reify-arguments stale-type hole** (fixed): after
   `reifyArgumentsImpl`, arm64 never widened the lazy-arguments FR's
   recorded type past its pre-reification `FRType::OtherNonPtr`, so a
   later type-sensitive fast path could trust a stale "non-pointer"
   claim. x86-64 hit this under `-Xjit-emit-type-asserts` and fixed it
   locally (`lib/VM/JIT/x86-64/JitEmitter-array.cpp`,
   `reifyArgumentsImpl`); arm64 was originally left as-is to keep its
   emission byte-identical to its stored baseline. Fixed 2026-08-26 by
   mirroring the x86-64 line; regression test
   `test/jit/reify-arguments-type.js` runs on both architectures. The
   fix is compile-time type bookkeeping only -- `jit-diff.sh` against
   both stored baselines shows the fix adds no change to any
   previously-existing function's emitted code (the only diff on
   either architecture is the new test's own dump, appended once as
   the file joins the default corpus).
3. A static_assert message-form inconsistency across both backends'
   `JitEmitter-*` files (some bare-condition, some message-form, no
   pattern to which) -- a style sweep, not a correctness issue.
   Swept 2026-08-26: every `static_assert(cond && "msg")` now uses the
   two-argument form, and the arm64 `HVTag_Str` message was corrected.
   Bare one-argument asserts (e.g. `static_assert(HERMESVALUE_VERSION ==
   2)`) were left as-is.

**x86-64 dump baseline and the byte-identical-refactor workflow.**
`utils/jit/jit-dump.sh` / `jit-diff.sh` now cover x86-64 with no
architecture flag: the canonicalizer's rules are keyed on the disassembly
syntax each backend emits (arm64's `mov x.., 0x..` / `ldr .., [RO_DATA]` /
`.xword`; x86-64's `.dq`), so both sets coexist in the same script. x86-64
turned out to need *fewer* new rules than expected. Its `loadBits64InGp`
(`lib/VM/JIT/x86-64/JitEmitter.h`) is an unconditional `mov reg, imm64` --
no `isCheapConst()` split, no RO-data fallback for GP constants -- so the
thing that makes two arm64 runs of the *same* binary diverge doesn't exist
on this backend. What still varies with ASLR (call targets, `runtimeModule`,
`codeBlock_`, property-cache addresses) always prints as `mov reg, 0x<8+
hex digits>`, confirmed against a real dump by diffing two raw runs over
`test/jit/*.js` and `test/jit/x86-64/*.js`: every differing line matches
that shape, none shorter. The pre-existing trailing `s/0x[0-9A-Fa-f]{8,}/
ADDR/g` rule already collapsed all of them; the only addition was
`/^\.dq /d`, dropping the RO_DATA payload listing exactly as `/^\.xword /d`
does on arm64 (the `[RO_DATA]`/`[RO_DATA+N]` references in code carry no
embedded hex, so only the listing needed it).

That trailing rule is width-only, not origin-aware, which is a real
limitation and not just a theoretical one: a `mov reg, imm64` whose
immediate is a genuine 64-bit constant -- a NaN-boxing tag mask, an
IEEE 754 double's bit pattern -- prints at 8+ hex digits just like an
ASLR-moved pointer, and gets collapsed to `ADDR` the same way. A change
to such a constant is therefore invisible on the instruction line; it
surfaces only as a comment-line diff, which `--comments-ok` suppresses.
See `utils/jit/README.md`'s "Limitations" section for the full writeup.

The workflow mirrors arm64's exactly:

```sh
cmake --build cmake-build-x86jit --target hermes
cp cmake-build-x86jit/bin/hermes /tmp/hermes-x86-before
# ... make an emitter change ...
cmake --build cmake-build-x86jit --target hermes
utils/jit/jit-diff.sh /tmp/hermes-x86-before cmake-build-x86jit/bin/hermes
```

`cmake-build-x86jit/jit-baseline.dump` is the x86-64 analogue of arm64's
`cmake-build-arm64/jit-baseline.dump` (both untracked build artifacts,
recaptured with `jit-dump.sh -o ... bin/hermes` after an intentional
change). Two runs of an unchanged x86-64 binary produce byte-identical
canonicalized dumps, same as arm64; verified over both the default
`test/jit/*.js` corpus and the `test/jit/x86-64/*.js` corpus, and the
`jit-diff.sh` prove-can-fail check (a one-instruction operand-order swap
in `mulN`'s fast path, `vmulsd(res, dl, dr)` -> `vmulsd(res, dr, dl)` --
value-transparent under IEEE 754 commutativity, so it cannot itself hang or
misbehave at runtime, but the differently-encoded instruction shows up
cleanly in `jit-diff.sh` as 62 changed lines, `RESULT: INSTRUCTIONS
CHANGED`) confirms the tool still catches a real emitter change on this
backend. arm64 is unaffected: the extended script still produces
byte-identical arm64 self-compares, and `mulN`'s arm64 body was never
touched.

To build:

  cmake -B cmake-build-x86jit -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
    -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" -DHERMESVM_ALLOW_JIT=2

**The heap-value-mode build matrix.** The recipe above is
`HEAP_HV_MODE=HEAP_HV_64`, in which every compressed-pointer and
boxed-double branch of the emitter compiles to nothing. Two further trees
compile those branches, and both must be built and run for any change
that touches heap value width. The rule is not a fixed helper list: it
is any code that computes a heap slot offset or a heap-value width,
since `sizeof(SHGCSmallHermesValue)` changes across modes and so does
that arithmetic's result. That covers the obvious decode/encode helpers
-- `emit_load_cp`/`emit_store_cp`/`emit_sh_cp_{en,de}code*`,
`Emit_sh_shv_decode`, `emit_shv_string`, the `newObjectWithBuffer`
inline-versus-slow choice, `fastArrayLength`/`fastArrayLoad` -- and, just
as much, sites that only look like plain address arithmetic: the
property IC's slot addressing (`GetByIdImpl::emitLoadFromSlot`, the
generic tier's shift/bias, `getOwnBySlotIdx`), the object-literal buffer
visitor, and `emit_load_shv`/`emit_store_shv`/`loadSmallHermesValueInGpX`.
Treat the names above as examples of the rule, not the list to check
against.

(Deliberate backend drift: x86-64 carries two Class-D type-assert sites,
in `fastArrayLength`/`fastArrayLoad`, that arm64 lacks; this is
design-sanctioned for now, to be reconciled deliberately later.)

The inline write barrier (see "Inline write barrier" above) is
one more thing this matrix has to cover, and one of the few that is not
just an encoding difference: the PutById and PutByVal inline tiers exist in
all three trees, but under boxed doubles they encode the value into a
SmallHermesValue first and decline the doubles that would have to be boxed,
and under compressed pointers every slot address is scaled by four rather
than eight. `test/lit.cfg`'s `%hv-mode` substitution is what lets one
emitted-code test pin all of that per mode, under a `-HV64` / `-HV32` /
`-BOXED` FileCheck prefix, instead of a mode-gating lit feature that would
skip two of the three.

Build and run both:

  cmake -B cmake-build-x86jit-hv32 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
    -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
    -DHERMESVM_ALLOW_JIT=2 -DHERMESVM_HEAP_HV_MODE=HEAP_HV_PREFER32

  cmake -B cmake-build-x86jit-boxed -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DHERMES_ENABLE_ADDRESS_SANITIZER=ON \
    -DCMAKE_CXX_FLAGS="-O1" -DCMAKE_C_FLAGS="-O1" \
    -DHERMESVM_ALLOW_JIT=2 -DHERMESVM_HEAP_HV_MODE=HEAP_HV_BOXED

On x86-64, `HEAP_HV_PREFER32` defines `HERMESVM_COMPRESSED_POINTERS`,
`HERMESVM_BOXED_DOUBLES` and `HERMESVM_CONTIGUOUS_HEAP` together (the JIT
`#error`s on compressed pointers without a contiguous heap, so the iOS
segment-table encoding is out of scope for this backend), while
`HEAP_HV_BOXED` defines only `HERMESVM_BOXED_DOUBLES` and therefore
isolates the boxed-double decode from pointer compression. All three modes
run the same `test/jit/x86-64` suite; `test/jit/x86-64/hvmodes.js` exists
for the matrix and is the one test whose emitted code differs deliberately
in all three, covering every SmallHermesValue tag through the GetById
decode, the nullable compressed-pointer decode via `super`, and the single
place where the emitter picks a different code SHAPE per mode rather than a
different encoding of one shape -- a literal double that `canInlineDouble`
rejects sends `newObjectWithBuffer` to `newObjectWithBufferSlow`, which is
unreachable in HV64.

**Free-after-call invariant (read before adding call emitters).** On
x86-64, every temp-eligible register is caller-saved: all 8 GP temps
(`kGPTemp1`/`kGPTemp2`) and all 16 xmm registers (`kVecTemp`). Unlike
arm64, there is no callee-saved temp subset a call can leave alone.
Emitters must sync AND free all temps around any emitted call, not just
sync them -- a temp merely synced-but-not-freed still holds a pre-call
value the callee is free to clobber. Globals survive calls unharmed only
because they live in the callee-saved GPRs rbx/r12/r13 (`kGPSavedList`),
never in a temp. One safety net: under `-Xjit-emit-type-asserts`,
`readFRForAssert` prefers live registers over the frame, so a
sync-without-free bug that leaves a stale temp registered as an FR's
location produces a spurious assert trap rather than silently reading
garbage. That is a deliberate property, not a byproduct, and the
call-emitting milestone must preserve it.

- **asmjit already supports x86-64** and the vendored copy ships the backend
  (`external/asmjit/.../x86`). CMake currently compiles only `arm64/*.cpp`
  when targeting arm64; a parallel `lib/VM/JIT/x86-64/` tree is expected by
  `include/hermes/VM/JIT/JIT.h`.
- **Arch-independent pieces** usable as-is: DiscoverBB, RuntimeOffsets,
  PerfJitDump, JitHandlers, JitCounters, and the Compiler driver
  (JitCompiler.cpp) -- all now outside `arm64/`. A new backend implements
  the Emitter method surface and adds its branch to `JitCurArch.h`; the
  driver then compiles against it unchanged.
- **The file split is a map for the arch split.** The `JitEmitter-*.cpp`
  topical boundaries separate the register-file engine (`-regalloc`), the
  shared inline helpers (`-internal.h`), and lifecycle (`JitEmitter.cpp`)
  from the per-instruction emitters. The engine and the driver are the
  pieces worth *sharing* rather than reimplementing — the sync/spill
  invariants are subtle and have already produced bugs; their only real
  arch dependencies are "move reg↔reg" and "load/store a frame slot".
- **Register budget** is the main design pressure: arm64 gives the JIT 8
  callee-saved GpX globals (x21–x28) + 8 callee-saved VecD (d8–d15) + 16 GP
  temps + 24 vec temps. SysV x86-64 has only 6 callee-saved GPRs total
  (rbx, rbp, r12–r15, minus two for Runtime/Frame → 3–4 usable globals) and
  **no callee-saved XMM registers**, so number FRs cannot live in global vec
  registers across calls. Options: fewer globals + more aggressive
  temp usage, or saving XMMs manually around helper calls. The FR model
  (frame slot + optional global + optional locals) ports unchanged; only
  `kGPSaved`/`kVecSaved`/`kGPTemp`/`kVecTemp*` and the class split need
  rethinking.
- **Two-address arithmetic and flags**: the emitter callbacks in the
  DECL_BINOP/DECL_COMPARE tables take (res, left, right) three-address form;
  x86 needs mov+op sequences. Condition codes: the NaN-aware condition-code
  choices (kMI/kLS for `<`/`<=` on arm64) map to x86 as `ucomisd` with
  swapped operands and the above/above-equal (unsigned) conditions, which
  are false on unordered (CF=1), unlike below/below-equal;
  every DECL_COMPARE/DECL_JCOND entry must be re-derived, including inverted
  forms.
- **movk-style tag manipulation** (`emit_sh_ljs_object` etc.) needs x86
  equivalents (mov imm64 + or/and, or bit-manipulation via BMI where
  available); the 48-bit-shift tag tests (`asr #47`/`cmn`) become
  `sar $47` + `cmp`.
- **setjmp/longjmp exception protocol** and the whole SHJmpBuf/catch-table
  design are arch-independent (SHJmpBuf size differs).
- **Runtime calls are mov-imm64 + `call reg`** — the thunk machinery was
  measured, disabled, and deleted (see "Making calls to the runtime"); do
  not resurrect it for x86 without new measurements.
- **Reserve a non-allocated scratch GPR** (r11, optionally r10) mirroring
  x16/x17's role: call-target/IP materialization, encoding fallbacks, and
  emitted-code instrumentation all need scratch that must not perturb the
  allocator. Decide this before the temp allocator's register set is fixed.
- **IP materialization** (`getBytecodeIP`) and pc-relative RO data (`adr`)
  become RIP-relative `lea`.

**CI recipes, release-build validation and perf sanity (milestone 6,
Task 3).** A fifth build configuration joins the four above:
`cmake-build-x86jit-rel` (Release, clang, `HERMESVM_ALLOW_JIT=2`, no
ASan) — the first non-ASan, non-Debug x86-64 build exercised in this
port, and therefore the first run where `NDEBUG` strips every assert
(including ones with potential side effects, e.g. the rspDelta
bookkeeping counter and the per-instruction invariant checks). It builds
clean and passes both standing gates with no NDEBUG-only divergence:
`LIT_FILTER="jit/"` matches the ASan trees exactly (72 pass /
1 unsupported), and the `aarch64/jit-stress.js` differential is
byte-identical to the interpreter in all three variants (`-Xjit=force`,
plus `-Xjit-emit-type-asserts`, and `-O0`), with all 31 functions
compiling and zero declines per `-Xdump-jitcode=2`. A first perf sanity
pass on this Release build times interpreter vs. `-Xjit=force` on a hot
numeric loop, a scaled-up `stress.js`-shaped workload, and a
property/IC-heavy loop: the JIT wins on all three (1.45x-2.25x),
clearing the spec's "must not lose to the interpreter on hot loops" bar
with no tuning attempted.
**Full commands, the complete CI-shaped matrix for all five configs
(exact configure+test invocations, which gates each carries, the
dump-baseline workflow for both architectures, and the full perf table)
are in `doc/JITTesting.md`** — that document is the CI/perf companion to
this architecture writeup.

## Appendix: findings from the 2026-08-15 review

The whole JIT was reviewed (all of `lib/VM/JIT/**`) while producing this
document. Findings below are ordered by severity; each was verified against
the actual code, not just pattern-matched. Line numbers refer to the
`x86-jit` branch base (`origin/static_h` @ 14112ce36).

**Runtime verification pass (2026-08-15, arm64 macOS).** The original review
was done by reading code on an x86-64 host, where the JIT cannot run. Findings
1-6, 8 and 19 have since been re-tested by actually executing the JIT on
arm64 macOS (ASan+Debug, `-DHERMESVM_ALLOW_JIT=2`, `-Xjit=force` / `-Xjit=on
-Xjit-threshold=1`). Each finding carries a **Status** line below. The
code-level claims held up in every case; three *reachability* claims (2, 3
and 6) did not. Encoding failures show up as a whole-function JIT bail (use
`-Xdump-jitcode=2` for compile status and `-Xjit-crash-on-error` to turn the
silent bail into a hard stop). 7 and 9-14 cannot be constructed from JS under
current invariants and stay unverified; 15 is Linux `perf` jitdump and cannot
be exercised on an arm64 macOS host at all.

Note when testing: the tests under `test/jit` are gated on the lit feature
`jit_enabled == "2"`, i.e. they are silently skipped as "Unsupported" unless
the build is configured with `-DHERMESVM_ALLOW_JIT=2`. A `=1` build runs none
of them.

### High severity

1. **`_jit_call_builtin` never initializes the callee frame's ThisArg slot**
   (`JitHandlers.cpp:479-505`). The interpreter's `implCallBuiltin` writes
   `newFrame.getThisArgRef() = undefined` explicitly
   (`Interpreter-slowpaths.cpp:1000`) because `HBCISel::verifyCall` asserts
   CallBuiltin's `this` argument is a non-register-allocated
   `LiteralUndefined` — the slot is never populated by bytecode. The JIT
   handler writes PreviousFrame/SavedIP/SavedCodeBlock/SHLocals/ArgCount/
   NewTarget/CalleeClosureOrCB but not ThisArg, and the emitter deliberately
   skips it ("CallBuiltin internally sets 'this'", `JitEmitter.cpp:6535`).
   Any builtin that reads `this` — e.g. `Array.from`, whose step 1 uses
   `this` as the constructor — observes whatever value the caller's frame
   happened to hold in that slot. Observable semantic divergence from the
   interpreter (not memory-unsafe: the slot always holds a valid rooted
   value). One-line fix in the handler.

   **Status: CONFIRMED, reproduced.** Every call in a function shares the one
   outgoing ThisArg slot at the fixed index `nRegs +
   StackFrameLayout::ThisArg` (see the `#ifndef NDEBUG` loop at
   `JitEmitter.cpp:6537`), so a *preceding ordinary call* in the same function
   deterministically leaves its `this` behind for the builtin to find. A
   function that calls `Ctor.method()` and then `Array.from(arr)` returns a
   `Ctor` instance under the JIT and an `Array` under the interpreter,
   because `arrayFrom` reaches `isConstructor(C)` → `Construct(C)`
   (`Array.cpp:5317-5321`). Fixed by writing `getThisArgRef() = undefined` in
   the handler; JIT and interpreter output then match.

   The "not memory-unsafe" note understates the impact: the consequence is
   that a builtin invokes a constructor chosen by whatever the caller last
   used as a receiver, i.e. user JS runs where the spec says `Array.from`
   allocates a plain array. Worth rating above "semantic divergence".

   The same defect exists outside the JIT: `_sh_ljs_call_builtin`
   (`lib/VM/StaticH.cpp`) omits the same slot, and the SH backend lowers
   CallBuiltin's `this` to an `ImplicitMovInst` that emits no code, so nothing
   writes it there either. That one is the more serious of the two — the AOT
   path ships on every architecture, while the JIT is opt-in and arm64-only.
   Reproduced with `shermes -fno-inline -fstatic-builtins -exec` using the
   same script; fixed alongside.

2. **Compile-time null deref in `GetByIdImpl::emitParentSpecialization`**
   (`JitEmitter.cpp:5163-5170`). The first `initHCLazyIDMayAlloc(...)` call
   is a GC safepoint (it may create/grow the `usedHCs` ArrayStorage); the
   argument of the *second* call re-reads `cacheEntry->clazz`, a
   `WeakRoot<HiddenClass>` that the GC may have cleared during that
   safepoint. `initHCLazyIDMayAlloc` dereferenced its argument unchecked
   → SIGSEGV during JIT compilation. Near-deterministic under
   `HERMESVM_SANITIZE_HANDLES` when the cached class is dead (but see the
   Status below — it is not). The caller's null checks all happen before the
   first safepoint; `emitObjectSpecialization` is safe by ordering. Fix:
   re-check for null after the first call, or make `initHCLazyIDMayAlloc`
   null-tolerant.

   *Both remedies have since been applied: the callee now returns 0 for a
   null class, which covers every call site including the two that were
   safe only by ordering, and the call site re-reads the WeakRoot to keep
   the safepoint visible where it matters. Line numbers in this item refer
   to the pre-fix revision.*

   **Status: CONFIRMED as a crash, but the reachability claim below it was
   wrong.** The unguarded deref is lethal: with a full collection forced at
   that safepoint (which the safepoint is permitted to perform), a script
   whose cache entry is a live negMatch with `numGoodChanges == 1` dies with
   `SEGV in Emitter::initHCLazyIDMayAlloc ← getByIdImpl ←
   Compiler::dispatch`. Re-checking `cacheEntry->clazz` after the first call
   fixes it; compilation then bails to the slow path, which is safe because
   `emitFastPath()` is immediately followed by `a.bind(slowPathLab)`
   (`JitEmitter.cpp:4925-4926`), so an early return falls through into the
   helper call.

   However, "near-deterministic under `HERMESVM_SANITIZE_HANDLES`" does not
   hold. 400 independent natural attempts under 100% handle sanitization
   (`-gc-sanitize-handles=1.0`) entered `emitParentSpecialization` 400 times
   and hit the window **zero** times. The reason is structural: `HiddenClass`
   is allocated `LongLived::Yes` (`HiddenClass.cpp:222`), so it lives in the
   old generation and its `WeakRoot` is cleared only by a *completed OG
   marking cycle*. The allocation inside `initHCLazyIDMayAlloc` triggers a
   young-gen collection, which never clears it. Handle sanitization perturbs
   YG behaviour, not OG marking completion, so it does not help reproduce
   this at all. The bug is a genuine latent crash that requires a concurrent
   Hades OG cycle to complete at that one allocation — rare, not
   sanitizer-reproducible.

### Medium severity

3. **`throwIfThisInitialized` misses the in-try temp sync**
   (`JitEmitter.cpp:4555-4582`; self-acknowledged TODO at 4560, since
   replaced by the fix — line numbers refer to the pre-fix revision). Every
   other throwing emitter syncs temps when `isInTry()`. If
   `ThrowIfThisInitialized` throws inside a try (double `super()` call in a
   try), the longjmp lands in a same-function catch handler that reads FRs
   from the frame — and observes stale values for any FR updated only in a
   temp since BB entry. (Functions with try have no global regs, so the
   temp→frame sync is exactly what is missing.)

   **Status: omission is real, but NOT observable — belongs in the
   latent/hardening bucket, not Medium.** No such stale FR can exist at that
   point. `callImpl` performs `syncAllFRTempExcept(FR())` +
   `freeAllFRTempExcept({})` before every call
   (`JitEmitter.cpp:6388-6389`), and `ThrowIfThisInitialized` is *always*
   emitted immediately after the super call: there is exactly one IRGen site
   (`ESTreeIRGen-legacy-class.cpp:748`), sequenced between the super call and
   the `StoreFrameInst` that publishes `this`. The call — a full sync point —
   therefore always sits between any user computation and the check. Tested
   with double `super()` straight-line, `super()` in a loop, three
   independent live values, a value derived from `this`, and a branchy shape:
   across all 7 emitted check sites the only FRs written in the window are
   `SelectObject`/`Mov` of the compiler's `this` temps, which are dead on the
   throwing path (the handler's first act is `Catch rN`, and `Mov r0, r5`
   comes after the check). Interpreter and JIT output are identical in every
   shape.

   Still worth fixing as hardening, because the only thing keeping it safe is
   instruction ordering the JIT does not control: if BCGen ever sank a pure
   computation past the super call into that window, the bug would become
   live. Fixed by adding the `isInTry()`-guarded sync (sync only, no free —
   see "Sync-only vs. sync-and-free" above).

4. **`newObjectWithBuffer` string-literal store lacks the displacement
   fallback** (`JitEmitter.cpp:~3318`). `visitStringID` calls
   `emit_store_shv` bare, while the adjacent `storeVal` wraps the same
   store in `EXPECT_ERROR` with a register-offset fallback. An object
   literal with a string value past indirect slot ~4090 raises an
   unexpected asmjit error and the whole function silently fails to JIT.

   **Status: CONFIRMED.** An object literal of 4200 string-valued
   properties fails with `InvalidDisplacement: stur x3, [x1, 32768]`,
   just past the 32760 ceiling of the scaled unsigned offset. The same
   literal with number values compiles successfully, since those go
   through `storeVal` and take its fallback -- a clean A/B for the
   missing fallback. The young-gen prop-count bail at the top of
   `newObjectWithBuffer` does not fire first, so the path is reachable.
   The function silently falls back to the interpreter, so results stay
   correct. **FIXED** by routing the string store through the same
   fallback `storeVal` uses.

### Low severity (graceful whole-function JIT bail, or defensive)

5. **`loadFromEnvironment` unencodable LDR offset** for slots ≥ ~4092
   (`JitEmitter.cpp:4000-4004`; `LoadFromEnvironmentL` carries a UInt16
   slot). Compilation of the function is abandoned.

   **Status: CONFIRMED.** `LoadFromEnvironmentL r0, r0, $4199` fails
   with `InvalidDisplacement: ldur x1, [x1, 33624]`. Note when building
   a repro: the obvious one, a closure reading thousands of captured
   variables, hits finding 19 below first, because such a function also
   needs thousands of frame registers. Decouple them with one
   never-called closure that captures the variables and a second, tiny
   closure that reads only the highest slot.

   **FIXED** by routing the load through `emit_load_from_base_offset`,
   which already picks a multi-instruction sequence when the
   displacement does not encode. `storeToEnvironment` was never
   affected: it passes the slot to a runtime helper in `w3`.
6. **`createThis` unencodable ADD immediate** on HV64 builds
   (`JitEmitter.cpp:4260-4262`): `SHReadPropertyCacheEntry` is 24 bytes, so
   `24 * cacheIdx` stops encoding at cacheIdx ≥ 171 (uint8 range goes to
   254). Compilation abandoned.

   **Status: CONFIRMED, but mis-scoped in two ways.** The failure is
   real: `InvalidImmediate: add x3, x3, 4104`, which is `24 * 171`, one
   step past the 4095 ceiling of the 12-bit ADD immediate.

   It is not specific to `createThis`. Five sites do the same
   `24 * cacheIdx` arithmetic (`createThis`, the `getByIdImpl` slow
   path, `JitEmitter.cpp:5325`, and two `Mem` offsets at 633 and 5134),
   and `GetById` reaches the ceiling first simply because it is far
   more common -- the observed failure was emitting
   `GetByIdShort r2, r1, $171, $87`.

   The trigger is also cheaper than "171 call sites" suggests. Cache
   indices are allocated per property *name*
   (`propertyReadCacheIndexForId_`, `ISel.cpp:255`), not per site, so
   200 `new Ctor()` sites all share index 1. What is needed is 171
   distinct property names in one function, which is plausible in
   generated deserializers or large configuration readers. That makes
   it more reachable than "Low severity" implies.

   **FIXED** at all three bare-`add` sites via a helper that splits the
   immediate. The two `Mem`-offset sites were already safe, since
   `emit_load_from_base_offset` picks a multi-instruction sequence.
7. **`loadParam` int32 overflow** (`JitEmitter.cpp:2381-2385`): absurd
   `LoadParamLong` indexes (≥2^28) hit signed-overflow UB / `hermes_fatal`
   at compile time, where emitting the slow-path `undefined` would be
   correct.

   **Status: FIXED, not verified.** The offset is now computed in 64
   bits, and an index too large to encode branches unconditionally to
   the slow path, which yields `undefined`. Not reachable from JS at
   any source size, so there is no test: `LoadParamLong` would need an
   operand above 2^28, meaning a function with 268 million parameters.
8. **String-switch fixup runs after the memory-limit bail**
   (`JIT.cpp:214-232` vs. the `usedSize > memoryLimit` nullptr return at
   314-322): writes `nullptr + labelOffset` into the RuntimeModule's
   `jitCodeTarget` tables. Latent today (code never installed, JIT then
   disabled) but UB and a trap if the JIT is ever re-enabled. Skip the
   fixup when compilation returned null.

   **Status: CONFIRMED.** With `-Xjit-memory-limit` tuned so the
   enclosing function compiles but the switch-bearing function does
   not, the fixup loop runs with `res == nullptr` and writes 12
   `jitCodeTarget` entries derived from a null base. Latent exactly as
   described: the code is never installed and the JIT disables itself,
   so nothing dereferences them in that run. **FIXED** by skipping the
   fixup when compilation returned null. The fixup has since been
   removed outright: string switches dispatch through a per-body jump
   table, and the module's tables hold no code addresses at all.
9. **`initHCLazyIDMayAlloc` assigns the permanent ID before the pin
   succeeds** and discards `push_back`'s status (`JitEmitter.cpp:1733-1751`).
   Unreachable today (ID cap ≪ ArrayStorage max), but the "id != 0 ⟹
   pinned" invariant is enforced only by luck.

   **Status: FIXED.** The class is pushed onto `usedHCs` first and the
   id assigned only if that succeeded; on failure the pending OOM is
   swallowed and 0 returned, which every caller already handles as "no
   id available". Since `push_back` can allocate and therefore move
   objects, `setLazyJITId` now goes through the pinned handle rather
   than the raw argument — a detail worth carrying into the port. Still
   unreachable, for the reason above; the invariant no longer rests on
   it.

### Latent / hardening (no constructible failure under current invariants)

10. **`newTypedObjectWithBuffer` missing `syncToFrame(frParent)`**
    (`JitEmitter.cpp:3458-3462`), unlike every sibling that passes an FR's
    frame address (cf. `newObjectWithBufferAndParent`, 3424-3427).
    Currently unobservable: only Number/NonPtr-class FRs have global regs
    (the stale-frame precondition), and every value a NonPtr parent FR can
    hold — stale or fresh — `dyn_vmcast`s to the same null parent handle in
    the handler. Should be added for uniformity.

    **Status: FIXED, not verified.** The sibling's exact sequence is now
    used, comment included. Unobservable for the reason above, so there
    is no test.
11. **`stringSwitchImm` missing `syncToFrame(frInput)`**
    (`JitEmitter.cpp:5751-5755`); the handler reads the frame slot.
    Currently protected by three unrelated invariants (no global regs for
    string-capable FRs; zero-filled frame; non-string ⇒ default case is
    also correct).

    **Status: FIXED, not verified.** The sync is now emitted. No test:
    each of the three invariants independently prevents an observable
    failure, so no input can distinguish the two versions.
12. **64-bit store into the 32-bit `ArrayStorageSmall::size`**
    (`JitEmitter.cpp:3214-3220`, `newObjectWithBuffer`): overwrites 4 bytes
    past the field; masked because every indirect slot (incl. slot 0) is
    subsequently written before any safepoint. Should be `str wTmp`.

    **Status: FIXED, not verified.** Now `str` of the `w` register. No
    test: the four bytes clobbered are the start of `storage[0]`, which
    the emitter overwrites before anything can observe it, so the two
    versions are indistinguishable from JS.
13. **Degenerate TypeOfIs masks** (`JitEmitter.cpp:5500-5552`, 5329-5343):
    an all-bits (or empty) `TypeOfIsTypes` mask emits no checks and leaves
    the `typeOfIs` result register uninitialized / `jmpTypeOfIs` never
    jumps. Presumably compiler-folded before reaching bytecode; deserves an
    assert.

    **Status: FIXED, not verified.** Handled rather than asserted, which
    is better than the remedy this item asked for. Both degenerate masks
    make the result independent of the input: an empty mask matches
    nothing, and an all-bits mask inverts to empty and matches
    everything. `typeOfIs` emits the corresponding constant and
    `jmpTypeOfIs` emits an unconditional branch, in both cases skipping
    the per-tag checks entirely. No test: a degenerate mask cannot be
    produced from JS, since the compiler folds such a comparison to a
    constant long before bytecode.
14. **`createClosure`/`createGenerator` clobber argument registers before
    `freeAllFRTempExcept`** (`JitEmitter.cpp:4069-4086`, 4143-4175) —
    benign today, violates the local-reg invariant across a window.

    **Status: FIXED, not verified.** The free now happens before the
    argument registers are written, which is the order every other
    call-emitting site uses. No test: the window closes before anything
    consults the allocator, so no input distinguishes the two versions.
    Exercised with `-Xjit-emit-asserts`, whose
    `assertPostInstructionInvariants` check passes either way.
15. **`PerfJitDump` carries `debugEntries_` from a failed compile into the
    next function's debug record** (`PerfJitDump.cpp:195-260`) — wrong line
    attribution in `perf report` after any compile failure.

    **Status: FIXED, not verified and not verifiable on this host.**
    `debugEntries_` is consumed by `writeDebugInfoRecord`, reached only
    through `writeCodeLoadRecord`, which a failed compilation never
    calls. A `discardPendingCodeComments()` entry point now clears them,
    called from both failure paths in `compileCodeBlock`: the
    memory-limit bail and the longjmp error path.

    This is the one item that cannot be exercised on arm64 macOS at all.
    `PerfJitDump.cpp` sits behind `HERMES_ENABLE_PERF_PROF`, which
    Config.h sets only on Linux and Android, and the file includes
    `<elf.h>`. The no-op stub and the two call sites do compile here;
    the real implementation was checked by extracting the added method
    into a scratch translation unit against the Linux branch of the
    header, which confirms the signatures agree but not that it
    behaves correctly. Someone should run it under `perf` on Linux.
16. Cosmetic: `fastArrayLoad` has a dead duplicate `syncAllFRTempExcept`
    (3582-3583); slow-path debug comments print uninitialized `frInput2`
    (bitNot, 6836) / `frInput1` (loadThisNS, 4337); `storeToEnvironment`
    always logs "StoreNPToEnvironment" (4014); the comment at line 89 says
    pointer tags start at 0xfc while `HVTag_FirstPointer` is −3 (0xfd) —
    the emitted code uses the enum and is correct.

    **Status: all FIXED.** 0xfc is `HVTag_Unused` (or `HVTag_RawHV32`
    under boxed doubles); `HVTag_First` is 0xf9 and `HVTag_FirstPointer`
    is the fifth, 0xfd. The dead duplicate was the weaker of the two
    syncs, so removing it changes nothing. No tests: none of these
    affect emitted code.

### Found later, while reviewing the fixes

These are not from the original sweep. They turned up in the review of
the fixes for 1-3 and are recorded here because they live in the same
code and the port would otherwise inherit them.

17. **A bailed parent specialization left the site with no inline cache
    at all.** `emitFastPath` returned unconditionally after
    `emitParentSpecialization`, on the reasoning that nothing useful
    follows one. That holds when the specialization is emitted, but the
    function has three silent bail-outs (no lazy id for the object
    class, the parent class WeakRoot cleared, no lazy id for the parent
    class), each emitting nothing — so the caller's return skipped the
    generic tier too, and the site compiled to an object tag check, a
    hidden class load, and then the slow-path helper on every
    execution. Not a correctness bug; the slow path was always reached.

    It is not a rare shape either: `initHCLazyIDMayAlloc` returns 0
    once `prevHCId` reaches `kHCIdOverflow`, and that counter is never
    reset for the lifetime of the `JITContext`, so once a program has
    interned 65535 hidden classes, *every* parent-specialization site
    compiled from then on degrades permanently.

    **Status: FIXED.** `emitParentSpecialization` reports whether it
    emitted anything and the caller skips the generic tier only when it
    did. All three bail-outs happen before the first instruction is
    emitted, which is now a stated precondition. Worth keeping in mind
    for the port: the "return early after the specialized tier" shape
    is a natural thing to copy, and it is only correct when the tier
    actually emitted something.

18. **`JITNumGetByIdSpec` counted attempts, not emissions.** The
    counter was incremented before calling either specialization
    emitter, so every silent bail inflated a statistic described as
    "number of GetById specialized fast paths emitted". This one has
    teeth because `test/jit/getbyid-fast.js` asserts on its exact
    value, so an inflating bail could mask a specialization that had
    stopped being emitted. **Status: FIXED** — both emitters report
    whether they emitted, and the counter follows that.

19. **`_loadFrame` / `_storeFrame` do not check the frame offset**
    (`JitEmitter.h:955-968`, both carrying `// FIXME: check if the
    offset fits`). The frame slot offset is `(index + FirstLocal) * 8`,
    so a function with more than about 4090 frame registers cannot
    encode its own frame accesses and fails to JIT.

    Found while verifying 4-6, not in the original sweep. It is the
    most general member of that family: 4, 5 and 6 are each one
    emitter's missing fallback, whereas this one covers every frame
    register access in the backend. It surfaces first for any test that
    tries to build a large environment, since the closure that captures
    N variables also needs N frame registers -- which is what makes the
    repro for finding 5 awkward.

    Reproduced as `InvalidDisplacement: stur x2, [x20, 33648]` while
    emitting `LoadFromEnvironment` in a function with ~4200 registers.
    Graceful, like the rest of the family: the function silently falls
    back to the interpreter.

    **FIXED.** Both helpers now check the offset and fall back to a
    register offset materialized in x16, which sits outside the
    register allocator. Note that the paired-store path in
    `syncAllFRTempExcept` already guarded itself with `isStpGpXImm`, so
    the codebase knew about this class of problem; the single-access
    path just never got the same treatment.

### Verified-sound design points (checked, not bugs)

- The NaN condition-code table (see "Arithmetic" section) is correct in all
  invert × slow-path combinations, including boxed-value routing via
  `b.vs` placement.
- `DiscoverBB` covers every Addr8/Addr32 operand in BytecodeList.def, plus
  Catch, both switch table forms, and exception-handler targets.
- `JITContext::markRoots` rooting only `usedHCs` is sufficient: emitted
  code never bakes a raw GC pointer (strings are re-loaded through the
  IdentifierTable lookup vector at run time; HCs are compared by pinned
  lazy IDs; caches/tables are C++ memory).
- `_jit_find_catch_target`'s frame-walk for the faulting IP, the jmpbuf
  push/pop pairing, and the `hasTry_ ⇒ RegClass::Other` interaction are all
  consistent.
- The prologue stack math keeps sp 16-aligned for every save-count parity;
  the prohibitInvoke `ldur` of NewTarget before the register-stack check
  reads caller-written, always-valid memory.
- `_jit_put_by_id` and `_jit_new_empty_object_for_buffer` follow the GC
  handle rules (values pinned via frame slots before safepoints).

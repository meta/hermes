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
| `include/hermes/VM/JIT/Config.h` | Decides `HERMESVM_JIT` from `HERMESVM_ALLOW_JIT` + platform. Requires arm64 and either no compressed pointers or a contiguous heap. On Apple platforms the JIT is additionally disabled *except* on macOS and Mac Catalyst, so arm64 macOS is a supported (and convenient) host for testing the JIT. |
| `include/hermes/VM/JIT/JIT.h` | Dispatches to the per-arch `JITContext`; provides a no-op `JITContext` when the JIT is disabled. Already contains an `#elif defined(__x86_64__)` branch expecting `hermes/VM/JIT/x86-64/JIT.h` (does not exist yet). |
| `include/hermes/VM/JIT/arm64/JIT.h` | Public `JITContext`: enablement, thresholds, `shouldCompile`/`compile`, counters, `markRoots`. |
| `lib/VM/JIT/JitCompiler.cpp` | `JITContext::Compiler`: per-opcode `emitXXX` methods that decode operands and forward to the `Emitter`. Drives BB-by-BB compilation. Arch-independent; compiled inside the arch namespace selected by `JitCurArch.h`. |
| `lib/VM/JIT/arm64/JIT.cpp` | `JITContext` housekeeping: construction, `setHCIdLimit`, `dumpCounters`, `markRoots`. |
| `lib/VM/JIT/arm64/JitEmitter.h` | The `Emitter` class definition: the FR/HWReg model, `TempRegAlloc`, the capture-based `SlowPath` class, and the full emitter method surface (which is effectively the porting contract). |
| `lib/VM/JIT/arm64/JitEmitter.cpp` | Emitter lifecycle: constructor, `enter`/`frameSetup`/`leave` (prologue/epilogue), `newBasicBlock`, the runtime-call plumbing (`callRuntime*`), slow-path/catch-table/RO-data emission, `initHCLazyIDMayAlloc`, `addToRuntime`. |
| `lib/VM/JIT/arm64/JitEmitter-regalloc.cpp` | The register-file engine: `getOrAllocFRIn*`, `movHW*`, sync/spill/free. |
| `lib/VM/JIT/arm64/JitEmitter-internal.{h,cpp}` | Shared emission helpers: the `emit_sh_ljs_*` tag/encoding helpers, `emit_load_from_base_offset`, the SHV decoder, `EMIT_RUNTIME_CALL*` macros (`.h`); cold non-member helpers, object/environment init, asmjit error handler and logger (`.cpp`). |
| `lib/VM/JIT/arm64/JitEmitter-{alloc,const,object,property,call,env,array,arith,control}.cpp` | The per-instruction emitters, split by topic: young-gen allocation; constant materialization; object construction; the property protocol (incl. `GetByIdImpl`); JS calls; environments/closures; arrays/iterators/arguments; arithmetic and comparisons; control flow, switches and throws. See `doc/specs/2026-08-18-jitemitter-split-design.md` for the split rationale and boundaries. |
| `lib/VM/JIT/arm64/JitImpl.h` | `JITContext::Impl`: the asmjit `JitRuntime` (owns executable memory), HC lazy-ID counter, and `usedHCs` root array. |
| `lib/VM/JIT/JitHandlers.{h,cpp}` | C++ helpers callable from emitted code that are JIT-specific (the generic ones are the `_sh_ljs_*` functions from the SH runtime). Arch-independent. |
| `include/hermes/VM/JIT/JitCounters.h` | The `JIT_COUNTERS` list and `JitCounter` enum. The counter array is ABI between the VM and emitted code; arch-independent. |
| `lib/VM/JIT/DiscoverBB.cpp` | Scans bytecode to find basic-block boundaries (branch targets, fallthroughs after branches, Catch, switch tables, exception handler targets). Arch-independent. |
| `lib/VM/JIT/RuntimeOffsets.h` | `offsetof` constants for fields the emitted code touches directly (Runtime, StackOverflowGuard, CodeBlock, JSFunction, HiddenClass, IdentifierTable, Hades young-gen fields...). Arch-independent. |
| `lib/VM/JIT/PerfJitDump.cpp` | Linux `perf` jitdump support. Arch-independent. |

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
with the cache-entry address. Writes have no inline fast path yet (explicit
TODO): they go through `_jit_put_by_id`, which implements cache-hit,
cached-add-transition (checking the RuntimeModule `AddPropertyCacheEntry`,
parent epoch and parent pointer) and falls back to
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
objects (environments, own slots) are *not* inlined — they call the
`_sh_ljs_*` helpers, which perform the Hades barriers in C++ (see the TODOs
about inlining write barriers). The one Hades-specific check emitted inline
is a weak-root *read* barrier guard: `newObjectWithBuffer` reads the cached
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

## x86-64 porting notes

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

**Open maintainer question: destination-FR exclusion from pre-call syncs in
try regions.** Both backends' `syncAllFRTempExcept` calls before a
potentially-throwing call inside a try exclude that instruction's own
destination FR from the sync (`frRes != ... ? frRes : FR()`, throughout
`JitEmitter-*.cpp`). Whether a *throwing non-call* instruction can have its
destination register still live at the catch handler depends on whether
register allocation's liveness computation treats exceptional edges as
uses -- if it does not, the exclusion could in principle be unsound.
`test/jit/x86-64/exceptions.js` does not construct a case exercising this
shape, so it is flagged here for maintainer confirmation rather than closed
by a test.

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

**What milestone 6 holds.** Reaching full opcode coverage is not the same
as being production-ready: `test/jit/*.js` (the arm64-authored suite) is
still gated to arm64 by `test/jit/lit.local.cfg` and has not been
un-gated for x86-64 -- the x86-64-specific tests under `test/jit/x86-64/`
(22 tests) are a parallel, independently written suite, not a replacement
for running the original one on this backend. There are no x86-64 golden
dumps analogous to arm64's `cmake-build-arm64/jit-baseline.dump`, no CI
recipe wiring the three heap-value-mode builds into a regular run, and no
performance pass at all -- every design choice so far has been "does it
compile and match", never "is it fast". Milestone 6 is where those close:
golden-dump capture and `jit-diff.sh`-based regression checking for
x86-64, un-gating `test/jit` (and reconciling whatever arm64-only
assumptions surface when the same files run against this backend), a CI
recipe that runs the three-mode matrix on every change, and a first perf
sanity pass. Counter support (`-Xjit-emit-counters`, NumCall/NumCallSlow)
already works and needs no further milestone-6 work. To build:

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
   fixup when compilation returned null.
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

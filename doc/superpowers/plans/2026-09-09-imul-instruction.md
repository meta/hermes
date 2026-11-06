# Math.imul Bytecode Instruction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `Imul` as an IR instruction and 3-register bytecode
opcode, lowered from 2-argument `Math.imul` calls under static
builtins, implemented in the interpreter, SH backend, and both JIT
backends.

**Architecture:** A standalone `ImulInst` created only by
LowerBuiltinCalls; a bit-op-shaped opcode with imul's OWN coercion
semantics (BigInt/Symbol throw; unsigned-multiply wrap); the
instruction stays unreachable until the lowering task lands atomically
with its full test matrix.

**Tech Stack:** C++17, HBC bytecode, asmjit (x86-64) + arm64 emitter,
lit/FileCheck.

**Spec:** doc/superpowers/specs/2026-09-09-imul-instruction-design.md
(Codex-reviewed, all findings CLOSED; binding). The repo skill
`add-ir-instruction` is the canonical file checklist for Task 1 —
Task 1's implementer MUST invoke it.

## Global Constraints

- Semantics are `ToInt32(left)` then `ToInt32(right)` (each may throw
  — BigInt and Symbol DO throw; each may run valueOf), then the
  product of the two int32s mod 2^32 as a signed int32 number. The
  C++ multiply is ALWAYS unsigned-32 with a signed reinterpretation
  (the shipped mathImul arithmetic, lib/VM/JSLib/Math.cpp ~363-367);
  a direct signed multiply is UB and forbidden.
- BYTECODE_VERSION is NOT bumped — the version is bumped at release
  time, not per change. NO task touches BytecodeVersion.h.
- The bit-op shared slow paths and helpers are BigInt-aware and must
  NOT be reused for coercion — imul gets its own.
- Both JIT backends are mandatory; each reuses its OWN bit-op operand
  machinery unchanged (the proofs differ between backends and both
  are correct because the multiply consumes only the low 32 bits).
- Every test RUN exercising the instruction passes
  `-fstatic-builtins`; baselines pass `-fno-static-builtins`
  explicitly.
- Runtime C++ (lib/VM/) implementers invoke `gc-safe-coding` first.
- 80 cols, 2-space indent; IR instruction docs go in doc/IR.md ONLY
  (no doc-comments in Instrs.h) per the skill.
- Build: `cmake --build /home/tmikov/work/hermes-x86-jit/cmake-build-x86jit --target hermes`;
  arm64: same with cmake-build-arm64; x86 jit suite:
  `(cd /home/tmikov/work/hermes-x86-jit && LIT_FILTER='jit/' cmake --build cmake-build-x86jit --target check-hermes)`;
  arm64 jit suite: same against cmake-build-arm64 (qemu-configured);
  full suite: check-hermes with no filter. Never bare `cd`.
- Commits end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ExvqpAhy7pehcdZf34z3dB`.

## File Structure

Task 1 (IR): doc/IR.md, include/hermes/IR/Instrs.def, Instrs.h,
IRBuilder.h, lib/IR/IRBuilder.cpp, lib/IR/IRVerifier.cpp,
lib/Optimizer/Scalar/TypeInference.cpp, lib/BCGen/HBC/ISel.cpp (stub),
lib/BCGen/SH/SH.cpp (stub). (No lib/BCGen/facebook/ exists in this
checkout — skip the Mins stub, note it in the report.)
Task 2 (HBC): include/hermes/BCGen/HBC/BytecodeList.def,
lib/BCGen/HBC/ISel.cpp, lib/VM/Interpreter.cpp
(+ slow-path file if one is warranted),
lib/VM/JIT/JitCompiler.cpp (EMIT_UNIMPLEMENTED stub only).
Task 3 (SH): lib/VM/Operations.cpp (helper), include/hermes/VM/
static_h.h, lib/BCGen/SH/SH.cpp.
Task 4 (JIT): lib/VM/JIT/JitCompiler.cpp (driver dispatch),
lib/VM/JIT/{x86-64,arm64}/JitEmitter.h (+ per-backend glue).
Task 5 (lowering + tests): lib/Optimizer/Scalar/LowerBuiltinCalls.cpp,
tests across test/Optimizer, test/BCGen (if conventional), test/hermes,
test/shermes, test/jit (+ per-arch).
Task 6: docs, perf smoke, validation.

---

### Task 1: The IR instruction

- [ ] **Step 1:** Invoke the `add-ir-instruction` skill and follow its
checklist EXACTLY for an instruction named `ImulInst`, with these
spec-fixed decisions:
  - Standalone `DEF_VALUE(ImulInst, Instruction)` (NOT a
    BinaryOperatorInst tag), placed near the call/builtin
    instructions or the arithmetic group — match the file's local
    organization and say which you chose.
  - Two operands `LeftIdx, RightIdx`; getters `getLeft()/getRight()`;
    `hasOutput() = true`.
  - Side effects (spec-exact): both operands statically number-typed
    → `SideEffect{}.setIdempotent()`; otherwise
    `SideEffect::createExecute()` (the ExecuteJS bit is load-bearing
    for FrameLoadStoreOpts; throw/read/write alone is WRONG).
  - `inferImulInst` returns the int32 numeric result type the bitwise
    ops' inference uses MINUS its BigInt component (read
    TypeInference.cpp ~194 and reproduce the number-only part).
  - Verifier: both operands present; no type restriction (any value
    coerces or throws at runtime).
  - doc/IR.md entry documenting the semantics incl. the BigInt/Symbol
    throw and evaluation order.
  - HBC ISel + SH codegen: `hermes_fatal` stubs FOR NOW (replaced in
    Tasks 2-3).
- [ ] **Step 2:** Build x86 + arm64; run the FULL suite (nothing
creates the instruction — green means no enumeration switch broke).
- [ ] **Step 3:** Commit — `IR: add ImulInst`

### Task 2: Bytecode opcode and interpreter

- [ ] **Step 1:** `DEFINE_OPCODE_3(Imul, Reg8, Reg8, Reg8)` in
BytecodeList.def beside BitAnd. Do NOT touch BytecodeVersion.h — the
version is deliberately not bumped (Global Constraints).
The new opcode immediately generates a
`Compiler::emitImul` declaration+dispatch in the shared JIT driver
(JitCompiler.cpp ~179/215), so THIS task must also add
`EMIT_UNIMPLEMENTED(Imul)` via the existing stub machinery
(JitCompiler.cpp ~617) — Task 4 replaces it with the real mapping.
Without the stub, neither JIT backend links at this boundary.
- [ ] **Step 2:** `HBCISel::generateImulInst` replaces the stub:
ordinary 3-register emission modeled on the bit ops' generate
functions.
- [ ] **Step 3:** Interpreter case beside the BITWISEBINOP uses
(Interpreter.cpp ~3216), NOT through their macro/shared slow path:
fast path when both operands are numbers — convert each with the
double→int32 idiom the bit ops' fast path uses, multiply per the
Global Constraint (unsigned mul, signed reinterpret), encode — slow
path: `toInt32_RJS(left)` + exception check, `toInt32_RJS(right)` +
exception check, same multiply. Follow the interpreter's existing
CAPTURE_IP/exception conventions for the slow calls; put the slow
path where the neighboring ops put theirs (a case-local lambda or a
doImulSlowPath in the slow-paths file — match whichever the
neighborhood convention favors and say which).
- [ ] **Step 4:** Build both backends; run the FULL x86 suite. The
inserted opcode renumbers its neighbors and may break golden/bytecode
tests — regenerate
FileCheckOrRegen tests via `update-lit` ONLY where you understand the
failure is the opcode renumbering, update others by hand, never weaken;
list every touched test in the report.
- [ ] **Step 5:** Commit — `HBC: add the Imul opcode`

### Task 3: SH backend

- [ ] **Step 1:** `_sh_ljs_imul_rjs(SHRuntime *, const SHLegacyValue *,
const SHLegacyValue *)` — declared in static_h.h beside the bit-op
helper declarations; DEFINED beside the bit-op helper definitions
(they live in lib/VM/Operations.cpp ~3302, not StaticH.cpp — confirm
and match, including whether the neighbors carry an `_inline`
variant). Body: fast double path + the ordered toInt32_RJS slow path
+ the unsigned-multiply arithmetic. Do NOT call the BigInt-aware
shared bit-op templates.
- [ ] **Step 2:** `generateImulInst` in SH.cpp replaces the stub,
emitting the helper call the way the bit ops' SH codegen emits theirs.
- [ ] **Step 3:** Build both backends + shermes; full x86 suite green;
commit — `SH: implement Imul`

### Task 4: JIT, both backends

- [ ] **Step 1:** Shared driver: REPLACE Task 2's
`EMIT_UNIMPLEMENTED(Imul)` stub with the real
`EMIT_BINARY_OP(Imul, imul)`-style mapping beside the existing
entries (JitCompiler.cpp ~758). (Until this step, functions
containing the opcode simply decline JIT compilation via the stub —
correct but slow; after it, both backends must implement the
emitter.)
- [ ] **Step 2:** x86-64: add Imul to the `DECL_BIT_BINOP` table
(JitEmitter.h ~727) with an `imul r32, r32` callback and
`_sh_ljs_imul_rjs` as the slow call, reusing the table's operand
proof and signed re-encode UNCHANGED. If the table's parameter shape
doesn't fit (e.g. rcx placement flags), adapt minimally and explain.
- [ ] **Step 3:** arm64: same shape in its table with `mul w, w, w`.
Its operand proof (fcvtzs + sign-extension round-trip) stays
UNCHANGED — do not import a sentinel-style guard (fcvtzs saturates
and flushes NaN to zero; a sentinel compare would admit tagged
non-numbers).
- [ ] **Step 4:** Build both; x86 jit suite AND arm64 jit suite green
(nothing emits the opcode yet — this checks compilation and that no
dispatch assert fires); commit — `JIT: implement Imul on both backends`

### Task 5: Lowering + the full test matrix (the turn-on task)

- [ ] **Step 1:** LowerBuiltinCalls: where the pass rewrites a proven
`Math.imul` call, special-case `getNumArguments() == 3` (argument 0
is the receiver, discarded exactly as the CallBuiltin rewrite
discards it, ~line 172; no undefined-this precondition) →
`builder.createImulInst(arg 1, arg 2)` replacing the call; all other
arities keep the CallBuiltin rewrite.
- [ ] **Step 2:** Tests, all in this task, all green together:
  - test/Optimizer/: lowering test beside xmod-builtins.js — 2-arg →
    ImulInst; 0/1/3-arg → CallBuiltin; shadowed `Math.imul` → not
    lowered (one pinned case). `%FileCheckOrRegen` if the neighbors
    use it.
  - Bytecode: a `-dump-bytecode` test pinning the `Imul r, r, r`
    encoding (place per repo convention — with the Optimizer test's
    directory or test/BCGen/HBC, matching where similar opcode pins
    live; state which).
  - test/hermes/math-imul-instruction.js: the spec's full edge
    matrix (2^31 boundaries incl. (0x7fffffff,2) → -2 and
    (-0x80000000,-1) → -2147483648 (2^31 mod 2^32 reinterpreted
    signed — NOT 0)... COMPUTE each expected value carefully —
    verify every expectation against a stock engine or the
    interpreter-with--fno-static-builtins run; NaN/±Inf/±0 with
    Object.is sign pins; fractional truncation; valueOf print-order
    on both operands; string operands; BigInt and Symbol THROW
    tests, including one whose unused result at -O pins that DCE
    kept the throw; missing-second-arg staying CallBuiltin; the
    `((a|0)*(b|0))|0` reference agreement under 2^53 plus explicit
    big-product divergence cases). RUN lines: `-fstatic-builtins -O`
    exercising the instruction, plus a `-fno-static-builtins`
    baseline RUN with identical expected output.
  - test/shermes/: the matrix core, same two RUN flavors.
  - test/jit/: an arch-neutral interpreter-vs-JIT diff test modeled
    on putbyval-inline.js (both backends run it), compiled with
    -fstatic-builtins; plus per-arch emitted pins (the
    putbyval-inline-emitted.js / -emitted-arm64.js pattern) pinning
    the fast-path multiply instruction and the `_sh_ljs_imul_rjs`
    slow call.
  - Prove-can-fail for the headline pins: for the ARITHMETIC oracle,
    use a mutation with a guaranteed observable difference — corrupt
    the wrapped product (e.g. add 1) or encode it as UNSIGNED int32
    (signed-vs-unsigned MULTIPLY is not a valid lever: the low 32
    bits are identical, and this tree builds with strict-overflow
    optimization disabled, so nothing may change). For the LOWERING
    pins, mutate the arity check. Watch named checks fail, restore,
    REBUILD.
- [ ] **Step 3:** Full x86 suite + x86 jit suite + arm64 jit suite
green; commit — `Compiler: lower 2-arg Math.imul to the Imul instruction`

### Task 6: Validation, perf smoke, docs

- [ ] **Step 1:** Heap modes: jit suite on cmake-build-x86jit-hv32 and
-boxed (rebuild first); arm64 full jit suite; MallocGC build compiles
(cmake-build-x86jit-malloc, target hermes — the JIT tables are
GC-independent here but the build must stay green).
- [ ] **Step 2:** Perf smoke (scratch, not committed): an imul-heavy
loop (e.g. a 32-bit hash/mix over an array). EACH revision compiles
the same source with ITS OWN compiler and runs its own artifact —
the candidate's bytecode contains the Imul opcode, which the
baseline binary does not know, and with no version bump NOTHING
rejects the mismatched artifact (it would misbehave silently), so
one shared .hbc cannot serve both:
baseline = throwaway worktree at the pre-plan tip, minimal Release
build, `-O -fstatic-builtins -emit-binary` there, run there;
candidate = cmake-build-x86jit-rel (rebuilt), same source, own
.hbc. 3 runs interpreter and `-Xjit=force` each side. Record all
numbers.
- [ ] **Step 3:** doc/JIT.md: a short paragraph in the appropriate
section (the bit-ops/arithmetic neighborhood) noting Imul joins the
bit-op tables on both backends; spec's Delivered section appended
with the measured numbers; doc/IR.md was done in Task 1. (A
deferred-bump dz issue was filed here during execution and later
deleted: bytecode versions are bumped at release time, not tracked
per change.)
- [ ] **Step 4:** Full non-JIT x86 suite once more; commit —
`docs: Imul instruction delivered`

## Self-Review Notes (plan time)

- Spec coverage: IR + effects/inference exactness (T1), opcode +
  interpreter-with-own-slow-path (T2), SH helper placement
  + non-BigInt coercion (T3), driver dispatch + both backends'
  unchanged proofs (T4), lowering arity/receiver + full matrix +
  activation flags + baselines (T5), validation + perf + Delivered
  (T6). Non-goals honored (no other builtins, no folding, no arity
  normalization).
- Green at every boundary: nothing creates ImulInst until T5;
  opcode-renumbering fallout is contained in T2's full-suite step.
- The two spec-mandated prohibitions (signed-multiply UB; BigInt-aware
  path reuse) appear in Global Constraints AND the owning tasks.
- Deliberately delegated with instructions: Instrs.def placement,
  slow-path location convention, bytecode-test directory, `_inline`
  variant question — each with "match the neighbors and report".

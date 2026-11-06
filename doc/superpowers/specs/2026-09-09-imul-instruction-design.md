# Math.imul as a bytecode instruction — design

Date: 2026-09-09. Status: approved design, pre-implementation.
Scope: full stack — IR, lowering, HBC bytecode + interpreter, SH
(shermes) codegen, both JIT backends. NO BYTECODE_VERSION bump: the
version is bumped at release time, not per change.

## Motivation

`Math.imul(a, b)` is `ToInt32(a) * ToInt32(b)` wrapped to int32 —
semantically a member of the bit-op family, whose members (BitAnd,
LShift, ...) are already 3-register bytecode instructions. imul alone
is a CallBuiltin: every call pays frame construction, builtin-table
dispatch, NativeFunction entry, NativeArgs parsing, and two full
`toInt32_RJS` calls, in the interpreter AND in the JIT (whose
`callBuiltin` emits one opaque helper call). It cannot be lowered to
existing ops: `(a|0) * (b|0) | 0` multiplies as doubles, and the
product of two int32s can exceed 2^53, losing exactly the low bits
imul's modular semantics require.

A dedicated instruction fixes every execution engine at once — the
interpreter (which is the whole story on iOS, where the JIT does not
run), shermes-compiled code, and both JIT backends — and deletes the
argument-marshalling bytecode around each call site. A JIT-only
CallBuiltin carve-out was considered and rejected: it helps one
engine on one platform.

## Design

### IR: ImulInst

A standalone instruction per the add-ir-instruction checklist (NOT a
`BinaryOperatorInst` tag — those map to JS source operators, and imul
has none). Two operands (left, right), produces a value.

imul is NOT semantically identical to the bit ops, and the difference
is BigInt: `1n & 1n` computes BigInt arithmetic, while
`Math.imul(1n, 1n)` THROWS (ToNumber of a BigInt is a TypeError), as
does a Symbol operand. So the instruction gets its OWN effect and
type summaries rather than mirroring the bit-op logic
(`BinaryOperatorInst::getBinarySideEffect`, Instrs.cpp ~180, declares
two-BigInt-operand bit ops side-effect-free — copying that would let
DCE delete an unused `Math.imul(1n, 1n)` that must throw):

- Side effects, with the exact constructors: when both operands are
  statically number-typed, `SideEffect{}.setIdempotent()` (an empty
  summary does not satisfy isPure()); otherwise
  `SideEffect::createExecute()` — the ExecuteJS bit matters, not just
  throw/read/write: FrameLoadStoreOpts invalidates cached
  captured-variable loads on ExecuteJS specifically, and a valueOf
  can mutate captured variables.
- Type inference: a dedicated `inferImulInst` returning the int32
  numeric result type the bitwise ops use for their NUMBER result,
  WITHOUT the BigInt component their inference includes
  (TypeInference.cpp ~194 is the reference for the type lattice
  spelling).

Created ONLY by the lowering below; IRGen never emits it.

### Lowering: in LowerBuiltinCalls

The pass that proves a call resolves to a builtin rewrites it to
`CallBuiltinInst` uniformly, with no knowledge of imul. A separate
peephole then turns any `CallBuiltinInst` for `Math.imul` into
`ImulInst`, and the pass runs it over every `CallBuiltinInst` it
walks — including the one it just created. Nothing in the compiler
emits a `CallBuiltin(Math.imul)` that survives to the backend.

Argument 0 is the receiver — the Math object on an ordinary call —
and is DISCARDED, exactly as the CallBuiltin rewrite discards it;
there is no undefined-this precondition on the original call.

Every arity is normalized. `Math.imul` reads exactly two arguments:
a missing one is undefined and ToInt32(undefined) = 0, so the
peephole pads with the literal `0` — the same value, but a literal
number leaves the padded operand statically numeric, which an
undefined never would. Arguments past the second are dropped from
the operand list; they were already evaluated by their own
instructions, which is precisely what the builtin does with them.

The static-builtins resolution condition is untouched: the
instruction appears exactly where CallBuiltin(Math.imul) would have
appeared, under the same proof.

Evaluation-order note: the instruction evaluates ToInt32(left) then
ToInt32(right), which is the same order the builtin performs them, so
observable valueOf side effects are unchanged.

### Constant folding: in InstSimplify

`ImulInst` on two literals folds to the product. The guard is that
`evalToInt32` must convert both operands, and it converts exactly the
literals whose ToInt32 runs nothing and cannot throw — numbers,
booleans, null, undefined. It refuses BigInt, whose ToInt32 throws a
TypeError, so that instruction survives to throw at run time; it also
refuses strings, which is a conservative miss rather than a
correctness requirement.

Because the padding is a literal `0`, calls with fewer than two
arguments fold too: `Math.imul()` becomes `0` rather than shipping a
call.

### HBC: opcode, interpreter, version

- `DEFINE_OPCODE_3(Imul, Reg8, Reg8, Reg8)` in BytecodeList.def,
  placed with the bit ops (near BitAnd).
- `HBCISel::generateImulInst` emits it; register allocation is the
  ordinary 3-register path the bit ops use.
- Interpreter: a case in the `BITWISEBINOP` NEIGHBORHOOD but NOT
  through its machinery — the bit-op shared slow path
  (Interpreter-slowpaths.cpp ~1200) and the shared Operations bit-op
  helpers are BigInt-aware and compute BigInt results, which imul
  must not do. Imul gets its own slow path: `toInt32_RJS(left)`
  (throws on BigInt/Symbol; may run valueOf), exception check, then
  `toInt32_RJS(right)`, exception check, then the multiply. The fast
  path (both operands doubles) converts each and multiplies.
  The multiply itself — everywhere in C++ — is the shipped
  `mathImul` arithmetic (Math.cpp ~363-367): multiply as UNSIGNED
  32-bit, then reinterpret the wrapped product as signed int32 before
  encoding. A direct signed 32-bit multiply is undefined behavior on
  overflow and is forbidden.
- `BYTECODE_VERSION` is NOT bumped: the version is bumped at release
  time, not per change. Adding an opcode can renumber neighbors and
  break golden bytecode tests; any such test is regenerated
  (FileCheckOrRegen) or updated by hand — never weakened.

### SH (shermes) backend

- `_sh_ljs_imul_rjs(SHRuntime *, const SHLegacyValue *a, const
  SHLegacyValue *b)` — extern-C, declared in static_h.h beside the
  bit-op helpers, implementing the full semantics (fast double path +
  the toInt32_RJS-in-order slow path, unsigned-multiply arithmetic as
  above). NOTE: the existing bit-op helper DEFINITIONS live in
  Operations.cpp (~3302), not StaticH.cpp, and are BigInt-aware —
  place the imul helper beside them but do NOT reuse their coercion
  path; follow their file placement and `_inline`-variant convention,
  whatever it is there.
- `generateImulInst` in SH.cpp emits a call to it.
- Mins: stub per convention (unimplemented).

### JIT, both backends

Three pieces, all mandatory:

1. The SHARED driver dispatch: a new opcode auto-generates a
   `Compiler::emitImul` call through the dispatch table
   (JitCompiler.cpp ~179); without a corresponding
   `EMIT_BINARY_OP(Imul, imul)`-style entry beside the existing ones
   (~758), NEITHER backend builds. This is driver plumbing, not
   emitter code.
2. Each backend adds the opcode to its bit-binop table
   (`DECL_BIT_BINOP` in x86-64 JitEmitter.h ~727; the arm64
   equivalent) with a multiply callback, REUSING each backend's OWN
   existing operand machinery unchanged. Correction of an earlier
   draft's claim: neither proof is "exact int32" — both convert
   through 64 bits (x86-64 via cvttsd2si + round-trip,
   JitEmitter-internal.h ~809; arm64 via fcvtzs + a 63-bit
   sign-extension trick, its ~450), so each accepts some
   beyond-int32 integers and their accepted ranges differ between
   backends. That asymmetry is FINE for imul because the multiply
   consumes only the LOW 32 BITS of each converted operand — which
   is exactly ToInt32 for every value either guard admits — and the
   slow path covers the rest. Do not import an x86-style
   sentinel-only guard to arm64: fcvtzs saturates and flushes NaN
   to zero, so a sentinel compare there would wrongly admit tagged
   non-numbers.
3. The op bodies: `imul r32, r32` (x86-64) / `mul w, w, w` (arm64)
   on the low 32 bits, re-encoded through the signed path the table
   already provides. Helper fallback: `_sh_ljs_imul_rjs`.

BOTH backends are mandatory: an unimplemented opcode makes the JIT
decline entire functions containing it, which would turn this
optimization into an arm64 pessimization.

## Non-goals

- No other builtins (Math.clz32, floor, sqrt...): imul alone, per
  decision. The lowering hook and the instruction pattern are the
  extension points when appetite returns.
- No IRGen changes, no new flags, no typed-mode special-casing.
- No removal of the `Math.imul` entry from Builtins.def. Nothing
  calls the builtin any more, but that entry is also what
  `Runtime::assertBuiltinsUnmodified` and `freezeBuiltins` walk to
  verify and freeze `Math.imul` — the very proof the lowering rides
  on — and `findBuiltinMethod` keys the recognition off the same
  enum. It stays.
- No InstSimplify rule beyond constant folding. ImulInst on two
  literals folds (see above); the algebraic identities -- imul(x, 0)
  is 0, imul(x, 1) is ToInt32(x) -- are not done, because neither is
  valid without first proving the other operand's coercion cannot be
  observed.

## Testing

- Optimizer: a folding lit test, test/Optimizer/simplify-imul.js —
  the constant matrix folds, and a BigInt operand does not.
- Optimizer: a lowering lit test beside test/Optimizer/
  xmod-builtins.js — Math.imul becomes ImulInst at every arity under
  static builtins, with no CallBuiltin surviving; shadowed
  Math.imul is not lowered (rides the existing proof — pin one case).
- BCGen: bytecode emission test pinning the Imul opcode and its
  registers.
- Interpreter execution (test/hermes/): edge-value matrix executed
  WITHOUT the JIT: (0x7fffffff * 2), (-0x80000000 * -1), 2^31
  boundaries, NaN/±Infinity/±0 operands (result-sign pinned via
  Object.is or 1/x — plain equality cannot see -0, and note the
  ToInt32 pipeline makes every imul result an integer, +0 where a
  naive double multiply would give -0), fractional truncation
  (imul(2.9, 3) === 6), objects with printing valueOf on BOTH
  operands (pins evaluation order), string operands, BigInt and
  Symbol operands THROWING — including with the result unused at -O,
  which pins that DCE cannot delete the instruction (the effect-model
  finding) — missing second argument (stays CallBuiltin → 0), and
  agreement with the reference `((a|0)*(b|0))|0` where the exact
  product stays below 2^53 (the outer |0 supplies the wrap the
  reference otherwise lacks: (0x7fffffff, 2) must give -2, not
  4294967294), plus explicit big-product cases where imul differs
  from any double arithmetic.
- ACTIVATION: every RUN line meant to exercise the INSTRUCTION passes
  `-fstatic-builtins` (or the source carries 'use static builtin') —
  without it LowerBuiltinCalls rejects Math calls and the tests
  exercise only the unchanged builtin, passing vacuously. Each
  execution test also keeps one baseline RUN with
  `-fno-static-builtins` (spelled explicitly — merely omitting the
  flag does not override a source directive under autodetection)
  pinning identical output through the ordinary-call path (with
  static builtins off there is no CallBuiltin either; the call goes
  through normal dispatch to the unchanged mathImul builtin).
- shermes execution test (test/shermes/) with the same matrix's core.
- JIT (test/jit/x86-64/ + the arm64 twin): interpreter-vs-JIT diff
  RUN pair over the matrix, plus a SPEC pin on the emitted multiply
  in the fast path and the `_sh_ljs_imul_rjs` fallback call.
- Full suite on HV64 ASan; jit suite on HV32/BOXED; arm64 jit suite
  runs under qemu per doc/JITTesting.md if that is the established
  gate, else arm64 build + its lit config as the sibling tests do.
- Perf smoke (not a committed benchmark): a scratch imul-heavy loop,
  precompiled WITH `-fstatic-builtins` (else it measures the builtin),
  Release, interpreter and JIT, before vs after, recorded in the
  spec's Delivered section; expect interpreter and JIT both to
  improve by integer factors.

## Delivered

Implemented in full per this design, across five commits on `x86-jit`
(tip `743397574`): `bbcf3c7d1` (IR: ImulInst), `ac36dec99` (HBC opcode),
`470969eb2` (SH helper), `385b235c4` (both JIT backends), `743397574`
(LowerBuiltinCalls lowering). `BYTECODE_VERSION` was NOT bumped, per the
branch's one-deferred-bump convention; the deferred bump is recorded as
a dz issue (see below) rather than performed here.

### Cross-mode validation (Task 6, Step 1)

At the tip above, rebuilt and re-run (not reused from Task 5):

- `jit/` suite on `cmake-build-x86jit-hv32` (HEAP_HV_PREFER32, ASan):
  102 passed, 4 unsupported, 0 failed.
- `jit/` suite on `cmake-build-x86jit-boxed` (HEAP_HV_BOXED, ASan):
  102 passed, 4 unsupported, 0 failed.
- `jit/` suite on `cmake-build-arm64` under qemu-user (after rebuilding
  `cmake-build-host` first, to avoid the stale-InternalBytecode trap):
  55 passed, 51 unsupported (x86-64-only test files and other
  architecture-gated tests, matching the existing pre-imul gating
  pattern), 0 failed.
- `cmake-build-x86jit-malloc` (MallocGC + JIT): `hermes` target builds
  clean; the JIT's opcode tables are GC-independent, as expected.

### Perf smoke (Task 6, Step 2)

Benchmark: a Murmur3-style 32-bit hash/mix (`mix32`) applying two
`Math.imul` calls per mix, run over a 64-entry array for 400000 outer
iterations (25.6M `Math.imul` calls total), source under `'use static
builtin'`. Baseline = throwaway git worktree at the pre-plan tip
`6ae5cb46c`, minimal Release build (`-DCMAKE_BUILD_TYPE=Release
-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
-DHERMESVM_ALLOW_JIT=2`); candidate = `cmake-build-x86jit-rel` rebuilt
at the tip above. Each side compiled the identical source with its OWN
`hermes -O -fstatic-builtins -emit-binary` and ran its OWN `.hbc` (the
baseline binary does not know the `Imul` opcode, so a shared artifact
cannot be used). All twelve runs (both binaries, both modes) produced
the identical result value `1909784631`, confirming behavioral parity.

Interpreter (`hermes -b <own.hbc>`), milliseconds:

| Run | Baseline (CallBuiltin) | Candidate (Imul) |
|-----|------------------------|-------------------|
| 1   | 3267                   | 2507               |
| 2   | 3272                   | 2516               |
| 3   | 3270                   | 2510               |
| **median** | **3270**          | **2510**           |

JIT (`hermes -b -Xjit=force <own.hbc>`), milliseconds:

| Run | Baseline (CallBuiltin) | Candidate (Imul) |
|-----|------------------------|-------------------|
| 1   | 2605                   | 1701               |
| 2   | 2726                   | 1699               |
| 3   | 2619                   | 1707               |
| **median** | **2619**          | **1701**           |

Speedup (baseline median / candidate median): interpreter **1.30x**,
JIT **1.54x**. Both engines improve, as expected for eliminating a
`CallBuiltin`'s frame construction, builtin-table dispatch, and
argument marshalling in favor of one dedicated instruction. This
falls short of the Testing section's "integer factors" prediction:
the smoke loop's array indexing and loop overhead dilute the per-call
win, so the measured ratios bound the whole-loop effect, not the
per-imul effect.

### Versioning note

`BYTECODE_VERSION` is untouched: the version is bumped as part of
making a release, not after every bytecode change.

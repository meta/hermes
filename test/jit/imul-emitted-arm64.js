/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -fstatic-builtins %s > %t.int
// RUN: %hermes -fno-inline -fstatic-builtins -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -fno-inline -fstatic-builtins -Xjit=force -Xdump-jitcode=3 %s | %FileCheck --check-prefix=SPEC %s
// RUN: %hermes -fno-inline -fstatic-builtins %s | %FileCheck --match-full-lines %s
// REQUIRES: jit
// REQUIRES: jit-arch-arm64

// That Math.imul is compiled to an inline `mul` on arm64, and what the
// emitted sequence consists of. imul.js is the file that runs the values on
// every backend; this one only looks at the instructions, which is why it is
// small. Its x86-64 counterpart is test/jit/x86-64/imul-emitted.js.
//
// -fstatic-builtins IS LOAD-BEARING. Without it LowerBuiltinCalls leaves the
// call as a builtin call, the JIT emits one opaque helper call, and every
// SPEC line below fails to match -- which is the point: this file is what
// notices if the lowering stops reaching the JIT. An unimplemented opcode
// would be worse still, since the JIT declines the whole function that
// contains one; that is why both backends had to be done together.
//
// -Xjit=force is enough: the tier reads no property cache and every one of
// its guards is dynamic, so it is emitted for every Imul site.
//
// NOT MODE-SHAPED. Nothing here touches a heap slot: both operands are read
// from the interpreter frame, which holds full-width HermesValues in every
// heap-value mode, and the result is written back the same way. (The arm64
// build in this matrix is HV64 only in any case -- see doc/JITTesting.md.)
//
// There is no -fno-static-builtins baseline here -- that comparison against
// the plain builtin is imul.js's job -- and no `handle_san` UNSUPPORTED
// directive either, unlike the putbyval sibling, because imul touches no
// heap slots.

function imul(a, b) {
  return Math.imul(a, b);
}

var s = 0;
for (var i = 0; i < 20; ++i)
  s = imul(s + i, 3);
print(s);
// CHECK: -1679879026

// SPEC-LABEL:imul:
// SPEC: // imul r{{[0-9]+}}, r{{[0-9]+}}, r{{[0-9]+}}
//
// The operand proof, once per operand, in the form the bit-op family
// already uses on this backend: convert the double toward zero, sign-extend
// the low 63 bits back over the result, convert back, and require equality.
// fcvtzs saturates rather than trapping and flushes a NaN to zero, so the
// sbfx is what keeps a saturated conversion from comparing equal; an
// x86-style sentinel compare would be wrong here. One b.ne per operand is
// enough, unlike x86-64's pair of exits: an unordered fcmp leaves Z clear,
// so a NaN operand takes the same branch.
//
// The guard admits some integers outside int32, and admits a different set
// of them than x86-64 does. That asymmetry is harmless for imul: the
// multiply below reads only the low 32 bits of each converted operand, and
// those are exactly ToInt32 of anything either guard lets through.
// SPEC: fcvtzs [[LEFT:x[0-9]+]], {{d[0-9]+}}
// SPEC: sbfx [[LEFT]], [[LEFT]], 0, 0x3F
// SPEC: scvtf {{d[0-9]+}}, [[LEFT]]
// SPEC: fcmp {{d[0-9]+}}, {{d[0-9]+}}
// SPEC: b.ne [[SLOW:SLOW_[0-9]+]]
// SPEC: fcvtzs [[RIGHT:x[0-9]+]], {{d[0-9]+}}
// SPEC: sbfx [[RIGHT]], [[RIGHT]], 0, 0x3F
// SPEC: scvtf {{d[0-9]+}}, [[RIGHT]]
// SPEC: fcmp {{d[0-9]+}}, {{d[0-9]+}}
// SPEC: b.ne [[SLOW]]
//
// The multiply itself: `mul` on the W halves, which is where the modular
// semantics come from -- the low 32 bits of the product are what the
// instruction leaves in the destination, and they are the answer whether
// the operands are read as signed or unsigned. The result is re-encoded
// with the SIGNED conversion, which is the whole difference between imul
// and a uint32 bit operation.
// SPEC: mul {{w[0-9]+}}, {{w[0-9]+}}, {{w[0-9]+}}
// SPEC: scvtf {{d[0-9]+}}, {{w[0-9]+}}
//
// And the fallback for everything the guard rejects: the two frame slots are
// passed by address to the runtime helper, which redoes both conversions in
// order and can throw.
// SPEC: [[SLOW]]:
// SPEC: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC: add {{x[0-9]+}}, {{x[0-9]+}}, {{(0x[0-9A-F]+|[0-9]+)}}
// SPEC: // call _sh_ljs_imul_rjs

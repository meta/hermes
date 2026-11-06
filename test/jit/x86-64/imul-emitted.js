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

// That Math.imul is compiled to an inline `imul` on x86-64, and what the
// emitted sequence consists of. imul.js is the file that runs the values on
// every backend; this one only looks at the instructions, which is why it is
// small. Its arm64 counterpart is test/jit/imul-emitted-arm64.js.
//
// -fstatic-builtins IS LOAD-BEARING. Without it LowerBuiltinCalls leaves the
// call as a builtin call, the JIT emits one opaque helper call, and every
// SPEC line below fails to match -- which is the point: this file is what
// notices if the lowering stops reaching the JIT.
//
// -Xjit=force is enough: the tier reads no property cache and every one of
// its guards is dynamic, so it is emitted for every Imul site.
//
// NOT MODE-SHAPED. Unlike the inline property and element stores, nothing
// here touches a heap slot: both operands are read from the interpreter
// frame, which holds full-width HermesValues in every heap-value mode, and
// the result is written back the same way. So there is a single expectation
// rather than one per %hv-mode.
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
// The operand proof, once per operand: convert the double to a 64-bit
// integer, convert it back, and require the round trip to be exact. An
// ordered mismatch lands in ZF and an unordered one (a NaN operand) in PF,
// so each operand needs both exits. This is the bit-op family's shared
// guard, unchanged -- it admits some integers outside int32, which is
// harmless here because the multiply below reads only the low 32 bits, and
// those are exactly ToInt32 of anything it lets through.
// SPEC: vcvttsd2si [[LEFT:r[a-z0-9]+]], {{xmm[0-9]+}}
// SPEC: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, [[LEFT]]
// SPEC: vucomisd
// SPEC: jne [[SLOW:SLOW_[0-9]+]]
// SPEC: jp [[SLOW]]
// SPEC: vcvttsd2si [[RIGHT:r[a-z0-9]+]], {{xmm[0-9]+}}
// SPEC: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, [[RIGHT]]
// SPEC: vucomisd
// SPEC: jne [[SLOW]]
// SPEC: jp [[SLOW]]
//
// The multiply itself: a 32-bit `imul`, which is where the modular
// semantics come from -- the low 32 bits of the product are what x86 leaves
// in the destination, and they are the answer whether the operands are read
// as signed or unsigned. The result is then re-encoded as a SIGNED int32,
// which is the whole difference between imul and a uint32 bit operation.
// SPEC: imul {{e[a-z0-9]+}}, {{e[a-z0-9]+}}
// SPEC: vcvtsi2sd {{xmm[0-9]+}}, {{xmm[0-9]+}}, {{e[a-z0-9]+}}
//
// And the fallback for everything the guard rejects: the two frame slots are
// passed by address to the runtime helper, which redoes both conversions in
// order and can throw.
// SPEC: [[SLOW]]:
// SPEC: lea {{r[a-z0-9]+}}, [{{.*}}]
// SPEC: lea {{r[a-z0-9]+}}, [{{.*}}]
// SPEC: // call _sh_ljs_imul_rjs

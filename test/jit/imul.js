/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -fno-static-builtins %s > %t.builtin
// RUN: %hermes -fno-inline -fstatic-builtins %s > %t.int && diff %t.builtin %t.int
// RUN: %hermes -fno-inline -fstatic-builtins -Xjit=force -Xjit-crash-on-error %s > %t.force && diff %t.builtin %t.force
// RUN: %hermes -fno-inline -fstatic-builtins -Xjit=force -Xjit-crash-on-error -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.forcea && diff %t.builtin %t.forcea
// RUN: %hermes -fno-inline -fstatic-builtins -Xjit -Xjit-threshold=4 -Xjit-crash-on-error %s > %t.warm && diff %t.builtin %t.warm
// RUN: %hermes -fno-inline -fstatic-builtins -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-asserts -Xjit-emit-type-asserts %s > %t.warma && diff %t.builtin %t.warma
// RUN: %hermes -fno-inline -fstatic-builtins %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Math.imul through the JIT, compared against the two implementations it has
// to agree with.
//
// THE FIRST RUN LINE IS THE ORACLE, and it is why this file cannot pass
// vacuously. With -fno-static-builtins nothing is lowered and every call
// below reaches the untouched Math.imul builtin; every other run compiles
// the same source with -fstatic-builtins, so it runs the Imul instruction
// instead -- interpreted, JIT-compiled from cold, and JIT-compiled after
// warming. All of them are diffed against the builtin's output. Drop
// -fstatic-builtins from any of them and it stops testing this change at
// all.
//
// ARCHITECTURE-INDEPENDENT. This file checks values, never instructions, so
// it runs on every backend. That the inline multiply is emitted at all is
// pinned per backend, by x86-64/imul-emitted.js and imul-emitted-arm64.js.
//
// BOTH SIDES OF THE OPERAND GUARD ARE EXERCISED. Each backend admits a
// double operand only if it survives a conversion round trip, and the two
// backends' accepted ranges are deliberately not the same; everything else
// -- the non-int32 doubles, the strings, the objects, the values with no
// ToNumber at all -- takes the _sh_ljs_imul_rjs slow call. The pair table
// below mixes the two so that a single compiled body sees both, which is
// also what makes the guard's own correctness visible: admitting a value it
// should have rejected changes a printed number here.

// Every pair is exercised by one compiled body, so the guard, the inline
// multiply and the slow call all run in the same register assignment.
var pairs = [
  // Plainly int32, the inline path.
  [3, 4], [-5, 12], [-5, -12], [1000003, 1000033],
  // The 2^31 boundaries.
  [0x7fffffff, 2], [-0x80000000, -1], [0x7fffffff, 0x7fffffff],
  [0xffffffff, 0xffffffff], [0x80000000, 1], [0x80000000, 2],
  [0x80000000, 0x80000000],
  // Products past 2^53, where no double arithmetic can produce the answer.
  [123456789, 987654321], [1234567890, -987654321], [0x40000001, 0x40000001],
  // Integral but outside int32: ToInt32 must wrap them.
  [4294967296, 5], [4294967297, 5], [-4294967295, 3], [1e21, 7], [-1e21, 7],
  // Not integral: the round-trip guard rejects these on both backends.
  [2.9, 3], [-2.9, 3], [1.9999, 2], [-0.5, 100], [2.5, 2.5],
  // No inline form at all.
  [NaN, 5], [Infinity, 5], [-Infinity, 5], [Infinity, Infinity],
  [-0, 5], [-1, 0], [-1, -0],
  ["5", "3"], ["0x10", 2], [" 7 ", 3], ["abc", 2], ["", 9],
  [null, 5], [undefined, 5], [true, 7], [false, 7],
];

function imul2(a, b) {
  return Math.imul(a, b);
}

// A zero result must always be a POSITIVE zero: ToInt32(-0) is +0, so imul
// cannot produce the -0 that the corresponding double multiply would.
function sweep(iters) {
  var out = "";
  var negZeros = 0;
  for (var k = 0; k < iters; ++k) {
    for (var i = 0; i < pairs.length; ++i) {
      var r = imul2(pairs[i][0], pairs[i][1]);
      if (Object.is(r, -0)) ++negZeros;
      if (k === iters - 1) out += r + ",";
    }
  }
  return negZeros + " " + out;
}
print("sweep", sweep(40));

// The same body, but reached only after the function has been JIT-compiled
// on int32 operands alone, so that the guard is what has to reject the
// stragglers rather than the compiler having seen them.
function narrow(a, b) {
  return Math.imul(a, b);
}
// The loop has to go THROUGH narrow: calling Math.imul directly here would
// warm the global function instead and leave narrow with the handful of
// calls below it, which is not enough to compile in threshold mode.
var acc = 0;
for (var i = 0; i < 200; ++i)
  acc = narrow(acc + i, 3);
print("hot-int", acc, narrow(7, 6));
print("hot-then-wide", narrow(2.9, 3), narrow("5", "3"), narrow(NaN, 4),
      narrow(0x7fffffff, 2));

// Conversions run left to right, and both of them run.
function tracer(name, v) {
  return {
    valueOf: function () {
      print("valueOf", name);
      return v;
    },
  };
}
function withObjects(a, b) {
  return Math.imul(a, b);
}
for (var i = 0; i < 40; ++i)
  withObjects(i, i + 1);
print("order", withObjects(tracer("left", 6), tracer("right", 7)));

// An exception out of the slow call has to unwind through JIT-compiled
// frames. The loop makes the function hot first so that it is the compiled
// body that throws.
function catching(a, b) {
  try {
    return String(Math.imul(a, b));
  } catch (e) {
    return e.name;
  }
}
for (var i = 0; i < 40; ++i)
  catching(i, i + 1);
print("throws", catching(1n, 2n), catching(1n, 2), catching(2, 2n),
      catching(Symbol("s"), 2), catching(2, Symbol("s")), catching(9, 9));

// And with the result unused, which is what the JIT sees when nothing
// consumes the multiply: the throw still has to happen.
function discard(a, b) {
  Math.imul(a, b);
  return "no-throw";
}
for (var i = 0; i < 40; ++i)
  discard(i, i + 1);
try {
  print("discard", discard(3n, 4n));
} catch (e) {
  print("discard", e.name);
}

// CHECK:sweep 0 12,-60,60,-691379869,-2,-2147483648,1,1,-2147483648,0,0,-67153019,671530190,-2147483647,0,5,3,375390208,-375390208,6,-6,2,0,4,0,0,0,0,0,0,0,15,32,21,0,0,0,0,7,0,
// CHECK-NEXT:hot-int 3834700 42
// CHECK-NEXT:hot-then-wide 6 15 0 -2
// CHECK-NEXT:valueOf left
// CHECK-NEXT:valueOf right
// CHECK-NEXT:order 42
// CHECK-NEXT:throws TypeError TypeError TypeError TypeError TypeError 81
// CHECK-NEXT:discard TypeError

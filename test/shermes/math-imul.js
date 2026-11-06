/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-inline -fstatic-builtins -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -fno-inline -fno-static-builtins -exec %s | %FileCheck --match-full-lines %s

// The core of the Math.imul matrix through the SH backend, which compiles
// ImulInst to a call to _sh_ljs_imul_rjs. The full matrix, with the cases
// that only the interpreter can reach, is test/hermes/math-imul-instruction.js.
//
// The first RUN line is the one that exercises the instruction: without
// -fstatic-builtins LowerBuiltinCalls declines Math calls and nothing here
// is lowered. The second spells -fno-static-builtins explicitly -- omitting
// the flag would not override a source directive -- and pins that the
// ordinary call path produces exactly the same output.
//
// The value matrix goes through imul() rather than calling Math.imul
// directly. shermes optimizes by default, so a direct call on constants is
// folded by InstSimplify at compile time and never reaches the backend at
// all; inside imul() the operands are parameters, which the folder cannot
// see, and -fno-inline keeps it a real call. Constant folding has its own
// test in test/Optimizer/simplify-imul.js -- what this file is for is the
// generated code, so the generated code has to be what runs.
//
// The calls that stay direct are the ones whose subject is Math.imul itself
// rather than a value: its arity, its coercion order, and its survival of
// DCE. A two-parameter helper would answer those questions about the helper.
// None of them fold anyway -- their operands are objects, BigInts, or absent.

function imul(a, b) {
  return Math.imul(a, b);
}

function ref(a, b) {
  return ((a | 0) * (b | 0)) | 0;
}

print("basic", imul(3, 4), imul(-5, 12), imul(-5, -12));
// CHECK:basic 12 -60 60

// The 2^31 boundaries, where a naive int32 multiply is wrong.
print("wrap", imul(0x7fffffff, 2));
// CHECK-NEXT:wrap -2
print("min", imul(-0x80000000, -1));
// CHECK-NEXT:min -2147483648
print("edges", imul(0x7fffffff, 0x7fffffff), imul(0xffffffff, 5),
      imul(0x80000000, 1), imul(0x80000000, 2));
// CHECK-NEXT:edges 1 -5 -2147483648 0

// ToInt32 keeps only the low 32 bits of an out-of-range double.
print("modulo", imul(4294967296, 5), imul(4294967297, 5),
      imul(1e21, 7));
// CHECK-NEXT:modulo 0 5 375390208

// NaN and the infinities convert to +0.
print("nan", imul(NaN, 5), imul(Infinity, 5),
      imul(-Infinity, 5), imul(undefined, 5));
// CHECK-NEXT:nan 0 0 0 0

// Every zero result is a positive zero, which plain equality cannot see.
print("zerosign", Object.is(imul(-1, 0), 0),
      Object.is(imul(-1, 0), -0), Object.is(-1 * 0, -0));
// CHECK-NEXT:zerosign true false true

// Fractions truncate toward zero; strings go through ToNumber.
print("coerce", imul(2.9, 3), imul(-2.9, 3), imul("5", "3"),
      imul("0x10", 2), imul("abc", 2));
// CHECK-NEXT:coerce 6 -6 15 32 0

// Objects convert with valueOf, left operand first.
function tracer(name, v) {
  return {
    valueOf: function () {
      print("valueOf", name);
      return v;
    },
  };
}
print("order", Math.imul(tracer("left", 6), tracer("right", 7)));
// CHECK-NEXT:valueOf left
// CHECK-NEXT:valueOf right
// CHECK-NEXT:order 42

// BigInt and Symbol have no ToNumber, so they throw rather than computing a
// BigInt result the way a real bitwise operator would.
function tryImul(a, b) {
  try {
    return String(Math.imul(a, b));
  } catch (e) {
    return e.name;
  }
}
print("throws", tryImul(1n, 2n), tryImul(1n, 2), tryImul(Symbol("s"), 2));
// CHECK-NEXT:throws TypeError TypeError TypeError

// The same throw with the result unused: nothing may delete it.
function discardedThrow() {
  Math.imul(3n, 4n);
  return "unreached";
}
try {
  print("dce", discardedThrow());
} catch (e) {
  print("dce", e.name);
}
// CHECK-NEXT:dce TypeError

// Every arity becomes the instruction: a missing argument is padded with the
// literal 0, and arguments past the second are dropped.
print("arity", Math.imul(), Math.imul(7), Math.imul(3, 4, 5));
// CHECK-NEXT:arity 0 0 12

// The padding is an operand, not a shortcut, through this backend too: a
// one-argument call still converts the argument it was given. The line above
// cannot see this -- its one-argument result is 0 either way.
print("one-arg", Math.imul(tracer("lonely", 6)));
// CHECK-NEXT:valueOf lonely
// CHECK-NEXT:one-arg 0

// Agreement with ((a|0)*(b|0))|0 while the exact product stays below 2^53,
// and the divergence above it -- the reason this cannot be expressed with
// the arithmetic operators the language already has.
print("agree", imul(1000003, 1000033) === ref(1000003, 1000033),
      imul(65535, 65537) === ref(65535, 65537));
// CHECK-NEXT:agree true true
print("diverge", imul(0x7fffffff, 0x7fffffff),
      ref(0x7fffffff, 0x7fffffff), imul(123456789, 987654321),
      ref(123456789, 987654321));
// CHECK-NEXT:diverge 1 0 -67153019 -67153024

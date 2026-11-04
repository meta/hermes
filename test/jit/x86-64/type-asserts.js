/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// -Xjit-emit-type-asserts on x86-64: every emitTypeAssert/emitTypeAssertFR
// call site ported for arithmetic, comparisons, branches and bit ops, plus
// the Class C global-register-write checks at instruction boundaries. The
// first RUN line establishes the baseline without the flag, and carries
// -Xjit-crash-on-error since nothing here declines (measured); the second
// re-runs with it and must print byte-identical output -- a passing check
// is silent, so a miscompiled check would show up only as a diff or a
// crash. The third RUN line makes sure every function below was in fact
// compiled, so the differential can never silently degrade into comparing
// the interpreter against itself.
//
// Restricted to arithmetic, comparisons, branches and bit ops on purpose --
// not because property access, calls or array literals fail to compile
// (they all do now), but to keep this file's shapes isolated to the type
// assert sites it is exercising.

function addNumbers(a, b) {
  var sum = 0;
  for (var i = 0; i < 100; ++i)
    sum += a * b - i;
  return sum;
}
print(addNumbers(3, 4));
// CHECK: JIT successfully compiled FunctionID 1, 'addNumbers'
// CHECK-NEXT: -3750

// Materializes a genuine boolean value into its own variable, reads it
// twice (the branch and the `+= 1`), then branches on it, exercising
// jmpTrueFalse's known-Bool tier (TypePred::IsBool). No print() inside the
// compiled function -- calls are not a supported opcode yet.
function boolCond(x, y) {
  var b = (x === y);
  var r = 0;
  if (b)
    r += 1;
  else
    r += 1000;
  if (b)
    r += 10;
  return r;
}
print(boolCond(3, 3));
// CHECK: JIT successfully compiled FunctionID 2, 'boolCond'
// CHECK-NEXT: 11
print(boolCond(3, 4));
// CHECK-NEXT: 1000

// The `null` operand is a compile-time-known OtherNonPtr constant, driving
// strictEqualImpl's raw-bit tier (TypePred::BitComparable).
function eqNull(x) {
  return x === null;
}
print(eqNull(null));
// CHECK: JIT successfully compiled FunctionID 3, 'eqNull'
// CHECK-NEXT: true
print(eqNull(3));
// CHECK-NEXT: false

// Same elision, but as a conditional jump: the `true` operand is a
// compile-time-known Bool constant, driving jStrictEqual's raw-bit tier
// (TypePred::BitComparable), a distinct emitter from strictEqualImpl.
function eqTrue(x) {
  if (x === true)
    return 1;
  return 0;
}
print(eqTrue(true));
// CHECK: JIT successfully compiled FunctionID 4, 'eqTrue'
// CHECK-NEXT: 1
print(eqTrue(false));
// CHECK-NEXT: 0
print(eqTrue(3));
// CHECK-NEXT: 0

// Class C: the boolean-typed values here earn a non-pointer-classed global
// register (FRType::UnknownNonPtr), whose write is checked against
// TypePred::NotPointer at the following instruction boundary. The numbers
// earn Number-classed global registers, checked the same way at many more
// sites, including inside the loop body.
function nonPtrGlobalReg(n) {
  var flag = false;
  var total = 0;
  for (var i = 0; i < n; ++i) {
    flag = (i % 2) === 0;
    if (flag)
      total += i;
  }
  return total;
}
print(nonPtrGlobalReg(10));
// CHECK: JIT successfully compiled FunctionID 5, 'nonPtrGlobalReg'
// CHECK-NEXT: 20

// Bit ops: the fast paths do not assert IsNumber on their operands (they
// guard with emit_double_is_int and fall back to the slow path instead,
// matching arm64). What this exercises is Class C again: acc is a
// Number-classed global register, checked the same way as above, but at
// many more sites because of the loop.
function bitLoop(n) {
  var acc = 0;
  for (var i = 0; i < n; ++i) {
    acc = (acc ^ i) & 0xff;
    acc = (acc << 1) >>> 0;
  }
  return acc;
}
print(bitLoop(20));
// CHECK: JIT successfully compiled FunctionID 6, 'bitLoop'
// CHECK-NEXT: 262

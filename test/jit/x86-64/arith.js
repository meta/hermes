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

// Number arithmetic: Add/Sub/Mul/Div and their N variants, Mod, Inc/Dec,
// Negate, ToNumber and ToNumeric. The first RUN line is the real check --
// the same program run by the interpreter and by the JIT must print the
// same thing, and carries -Xjit-crash-on-error since nothing here declines
// (measured). The second RUN line only makes sure the functions under test
// were in fact compiled, so the differential cannot degrade into comparing
// the interpreter against itself.
//
// Straight-line bodies only, so each value is printed by its own call
// rather than collected into an array -- not because branches, comparisons
// or array literals fail to compile (they all do now; see loops.js, cmp.js
// and arrays.js), but to keep this file's shapes isolated to the
// arithmetic opcodes it is testing.
//
// Mixed-type operands (true/null) take the runtime slow call, which is a
// call inside the compiled function, not a reason to decline it.

function add2(a, b) { return a + b; }
function sub2(a, b) { return a - b; }
function mul2(a, b) { return a * b; }
function div2(a, b) { return a / b; }
function mod2(a, b) { return a % b; }
function neg1(a) { return -a; }
function poly(a, b) { return a + b * 3.5 - 0.5; }
function incdec(a) { var x = a; x++; x++; x--; return x; }
// The old value of x++ is live, which is what makes this a ToNumeric.
function postinc(a) { var x = a; var y = x++; return y; }
// The ToNumbers make every operand a known number, so this is the AddN /
// SubN / MulN / DivN family plus a Mod with both operands known.
function numops(a, b) { var x = +a, y = +b; return x + y - x * y / y % x; }
function negn(a) { var x = +a; return -x; }

print(add2(7, 2));
// CHECK: JIT successfully compiled FunctionID 1, 'add2'
// CHECK-NEXT: 9
print(sub2(7, 2));
// CHECK: JIT successfully compiled FunctionID 2, 'sub2'
// CHECK-NEXT: 5
print(mul2(7, 2));
// CHECK: JIT successfully compiled FunctionID 3, 'mul2'
// CHECK-NEXT: 14
print(div2(7, 2));
// CHECK: JIT successfully compiled FunctionID 4, 'div2'
// CHECK-NEXT: 3.5
print(mod2(7, 2));
// CHECK: JIT successfully compiled FunctionID 5, 'mod2'
// CHECK-NEXT: 1
print(neg1(7));
// CHECK: JIT successfully compiled FunctionID 6, 'neg1'
// CHECK-NEXT: -7
print(poly(7, 2));
// CHECK: JIT successfully compiled FunctionID 7, 'poly'
// CHECK-NEXT: 13.5
print(incdec(5.5));
// CHECK: JIT successfully compiled FunctionID 8, 'incdec'
// CHECK-NEXT: 6.5
print(postinc(5.5));
// CHECK: JIT successfully compiled FunctionID 9, 'postinc'
// CHECK-NEXT: 5.5
print(numops(7, 2));
// CHECK: JIT successfully compiled FunctionID 10, 'numops'
// CHECK-NEXT: 9
print(negn(7));
// CHECK: JIT successfully compiled FunctionID 11, 'negn'
// CHECK-NEXT: -7

// Doubles that are not integers.
print(add2(0.1, 0.2));
// CHECK-NEXT: 0.30000000000000004
print(sub2(0.1, 0.2));
// CHECK-NEXT: -0.1
print(mul2(0.1, 0.2));
// CHECK-NEXT: 0.020000000000000004
print(div2(0.1, 0.2));
// CHECK-NEXT: 0.5
print(mod2(0.1, 0.2));
// CHECK-NEXT: 0.1
print(numops(0.1, 0.2));
// CHECK-NEXT: 0.30000000000000004

// Signed zero. Printing the value alone would not distinguish -0 from 0,
// so divide into it: negating +0 must produce -0.
print(1 / neg1(0));
// CHECK-NEXT: -Infinity
print(1 / neg1(-0));
// CHECK-NEXT: Infinity
print(1 / negn(0));
// CHECK-NEXT: -Infinity
print(1 / mul2(-1, 0));
// CHECK-NEXT: -Infinity

// Infinities and NaN.
print(div2(1, 0));
// CHECK-NEXT: Infinity
print(div2(-1, 0));
// CHECK-NEXT: -Infinity
print(mod2(1, 0));
// CHECK-NEXT: NaN
print(mul2(0, Infinity));
// CHECK-NEXT: NaN
print(sub2(Infinity, Infinity));
// CHECK-NEXT: NaN
print(String(add2(NaN, 1)));
// CHECK-NEXT: NaN
print(String(neg1(NaN)));
// CHECK-NEXT: NaN
print(String(numops(NaN, 1)));
// CHECK-NEXT: NaN

// Non-numbers: the fast path's NaN test routes these to the runtime.
print(add2(true, 1));
// CHECK-NEXT: 2
print(add2(null, 3));
// CHECK-NEXT: 3
print(sub2(true, 1));
// CHECK-NEXT: 0
print(mul2(true, 3));
// CHECK-NEXT: 3
print(div2(true, 4));
// CHECK-NEXT: 0.25
print(mod2(true, 4));
// CHECK-NEXT: 1
print(neg1(true));
// CHECK-NEXT: -1
print(incdec(true));
// CHECK-NEXT: 2
print(postinc(true));
// CHECK-NEXT: 1
print(negn(true));
// CHECK-NEXT: -1

// The same, with the non-number on the RIGHT. These pin the fact that the
// binary fast paths compare the two operands against each other
// (`vucomisd left, right`) rather than an operand against itself, as the
// unary ones do. A self-compare regression is invisible to the cases
// above -- the left operand is already NaN there, so the slow path is
// still taken -- but here the left operand is an ordinary number and the
// bad code would fall through and compute on the raw NaN-boxed bits.
print(add2(1, true));
// CHECK-NEXT: 2
print(sub2(1, true));
// CHECK-NEXT: 0
print(mul2(3, true));
// CHECK-NEXT: 3
print(div2(1, true));
// CHECK-NEXT: 1
print(mod2(5, true));
// CHECK-NEXT: 0
print(add2(3, null));
// CHECK-NEXT: 3
// Both operands non-numbers.
print(add2(true, true));
// CHECK-NEXT: 2

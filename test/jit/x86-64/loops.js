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

// Loops and branches that stay in machine code: every conditional branch
// below resolves inside the compiled function, with no return to the
// interpreter. The first RUN line is the real check -- interpreter and JIT
// must print the same thing, and carries -Xjit-crash-on-error since nothing
// here declines (measured). The second RUN line makes sure every function
// below was in fact compiled, so the differential cannot degrade into
// comparing the interpreter against itself.
//
// The bodies are chosen so that every branch form the emitter can produce
// appears at least once. Verified with -Xdump-jitcode=1; the resulting
// census is:
//
//   jCond:  j__less, j__less_n, j_not_less_n, j__less_equal,
//           j__less_equal_n, j_not_less_equal_n, j__greater,
//           j_not_greater, j__greater_equal, j__eq, j_not_eq
//   jStrictEqual:  raw-bit tier (seqBool), known-number tier (nanLoop),
//           general tier in both polarities (seqAny, sneAny)
//   jmpTrueFalse:  number tier both polarities (truthy), bool tier (every
//           loop whose condition is a comparison *value*), generic tier
//           both polarities (truthy)
//   jmp:    the if/else join in pick
//
// Notes on individual functions:
//
// countdown/countdownRem return one value each rather than the pair
// `[n, c]`: array literals are not a supported opcode yet, and a function
// the JIT declines would not exercise anything here.
//
// nanLoop is the NaN case: `v < 100` and `!(v < 100)` must be false and
// true respectively for v = NaN, which is what pins the inverted branch to
// a condition that is true on unordered.
//
// leCount/leCountN pin the operand swap for "less or equal": the emitter
// compares the reversed operands so that the condition can stay in the
// above family, which is the family that is false on unordered. Their
// `c > 1000` bail-out exists so that a comparison miscompiled in the
// swap's direction shows up as a wrong answer rather than as a hung test.
function sum(n) {
  var s = 0;
  for (var i = 0; i < n; ++i) s += i;
  return s;
}
function countdown(n) {
  var c = 0;
  while (n > 0) { n -= 1.5; c++; }
  return c;
}
function countdownRem(n) {
  while (n > 0) { n -= 1.5; }
  return n;
}
function findFirst(limit, threshold) {
  for (var i = 0; i < limit; ++i) {
    if (i * i >= threshold) return i;
  }
  return -1;
}
function nanLoop(n) {
  var s = 0;
  for (var i = 0; i < n; ++i) {
    var v = i % 3 === 0 ? NaN : i;
    if (v < 100) s += 1;
    if (!(v < 100)) s += 1000;
  }
  return s;
}
function leCount(a, b) {
  var c = 0;
  while (a <= b) { a += 1; c++; if (c > 1000) return -1; }
  return c;
}
function leCountN(a, b) {
  var x = +a, y = +b, c = 0;
  while (x <= y) { x += 1; c++; if (c > 1000) return -1; }
  return c;
}
function eqCount(a, n) {
  var c = 0;
  for (var i = 0; i < n; ++i) {
    if (i == a) c += 1;
    if (!(i == a)) c += 100;
  }
  return c;
}
function pick(a) {
  var r;
  if (a > 0) r = a * 2; else r = a - 1;
  return r + 1;
}
function truthy(a, n) {
  var c = 0;
  var x = +a;
  if (x) c += 1;
  var y = +n;
  if (!y) c += 10;
  for (var i = 0; i < n; ++i) {
    var v = i - a;
    if (v) c += 100;
    if (!v) c += 1000;
  }
  return c;
}
function seqBool(a, b, n) {
  var c = 0;
  for (var i = 0; i < n; ++i) {
    if ((a < b) === (b < a)) c += 1;
  }
  return c;
}
function seqAny(a, b, n) {
  var c = 0;
  for (var i = 0; i < n; ++i) {
    if (a === b) c += 1;
  }
  return c;
}
function sneAny(a, b, n) {
  var c = 0;
  for (var i = 0; i < n; ++i) {
    if (!(a === b)) c += 1;
  }
  return c;
}
print(sum(1000));
// CHECK: JIT successfully compiled FunctionID 1, 'sum'
// CHECK-NEXT: 499500
print(countdown(10));
// CHECK: JIT successfully compiled FunctionID 2, 'countdown'
// CHECK-NEXT: 7
print(countdownRem(10));
// CHECK: JIT successfully compiled FunctionID 3, 'countdownRem'
// CHECK-NEXT: -0.5
print(findFirst(100, 500));
// CHECK: JIT successfully compiled FunctionID 4, 'findFirst'
// CHECK-NEXT: 23
print(findFirst(10, 500));
// CHECK-NEXT: -1
print(nanLoop(30));
// CHECK: JIT successfully compiled FunctionID 5, 'nanLoop'
// CHECK-NEXT: 10020
print(sum(0));
// CHECK-NEXT: 0
print(countdown(-1));
// CHECK-NEXT: 0
print(leCount(1, 5), leCount(5, 1), leCount(1, 1));
// CHECK: JIT successfully compiled FunctionID 6, 'leCount'
// CHECK-NEXT: 5 0 1
print(leCountN(1, 5), leCountN(5, 1), leCountN(NaN, 5));
// CHECK: JIT successfully compiled FunctionID 7, 'leCountN'
// CHECK-NEXT: 5 0 0
print(eqCount(3, 10), eqCount(NaN, 10));
// CHECK: JIT successfully compiled FunctionID 8, 'eqCount'
// CHECK-NEXT: 901 1000
print(pick(2), pick(-2), pick(0));
// CHECK: JIT successfully compiled FunctionID 9, 'pick'
// CHECK-NEXT: 5 -2 0
print(truthy(0, 5), truthy(3, 5), truthy(NaN, 5), truthy(1, 0));
// CHECK: JIT successfully compiled FunctionID 10, 'truthy'
// CHECK-NEXT: 1400 1401 5000 11
print(seqBool(1, 2, 3), seqBool(1, 1, 3));
// CHECK: JIT successfully compiled FunctionID 11, 'seqBool'
// CHECK-NEXT: 0 3
print(seqAny(1, 1, 3), seqAny(1, 2, 3), seqAny(NaN, NaN, 3));
// CHECK: JIT successfully compiled FunctionID 12, 'seqAny'
// CHECK-NEXT: 3 0 0
print(sneAny(1, 1, 3), sneAny(1, 2, 3), sneAny(NaN, NaN, 3));
// CHECK: JIT successfully compiled FunctionID 13, 'sneAny'
// CHECK-NEXT: 0 3 3

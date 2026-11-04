/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes %s > %t.int && %hermes -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -O0 %s > %t.int0 && %hermes -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// Ordinary (non-fast) arrays: NewArrayWithBuffer for every literal that has
// at least one element, and NewArray for the empty literal and for the array
// a spread builds. The first RUN line is the real check -- interpreter and
// JIT must print the same thing. The second re-runs it with the type asserts
// on. The last two pin that the functions under test were in fact compiled,
// so the differential cannot degrade into comparing the interpreter against
// itself.
//
// Unlike object literals (see objects.js), array literals lower to the
// buffer form at -O0 too: an all-literal array is one NewArrayWithBuffer at
// both optimization levels, and one with a computed element is a
// NewArrayWithBuffer for the literal prefix followed by
// DefineOwnInDenseArray for the rest. Every function here therefore compiles
// at both levels, and the CHECK and CHECK0 lines are identical -- literally
// so: the two -Xdump-jitcode=2 runs produce the same sequence of compile
// events, and the CHECK0 pins are a line-for-line mirror of the CHECK ones.
// Because nothing declines in either, the four differential RUN lines carry
// -Xjit-crash-on-error, so a future decline aborts instead of quietly
// weakening this file into an interpreter-versus-interpreter comparison.
//
// The element reads and writes below are ordinary property access
// (GetByVal/PutByVal/GetByIndex), not the FastArray opcodes -- plain JS
// never emits those. fastarrays.js covers the FastArray family, which needs
// -typed.
//
// `global` USED TO DECLINE HERE and no longer does. The string literals in
// the top-level code needed LoadConstString, which arrived in milestone 5,
// so the printing below was done by the interpreter and this header called
// that out as the reason the printed values were trustworthy. The top level
// is compiled code now, and the oracle is what it always really was: the
// first RUN line's separate `%hermes %s` process, which runs the same
// program with no JIT at all.

function small() {
  return [1, 2, 3];
}

// One element of each kind the array literal buffer can hold: a small int, a
// string, a bool, null, and two doubles, one of which is negative zero's
// neighbour -0.25 rather than an integer.
function kinds() {
  return [1, "two", true, null, 3.5, -0.25];
}

// Forty elements, well past anything an inline allocation would handle, so
// the buffer path runs long.
function big() {
  return [
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
  ];
}

// A literal whose second element is not a literal: NewArrayWithBuffer covers
// the prefix and DefineOwnInDenseArray fills in the rest.
function computed(x) {
  return [1, x, 3];
}

// A hole behaves like the computed case for the buffer -- the literal prefix
// stops at the hole -- and the hole itself stays absent, which is checked
// below with `in`.
function holes() {
  return [1, , 3];
}

// NewArray with size zero.
function empty() {
  return [];
}

// NewArray again, this time non-empty: the spread's destination array is
// allocated with a size hint and filled by the arraySpread builtin.
function spread(a) {
  return [...a, 99];
}

function sum(a) {
  var s = 0;
  for (var i = 0; i < a.length; ++i)
    s = s + a[i];
  return s;
}

// Writes past the end in a loop, so the array grows repeatedly.
function fill(n) {
  var a = [];
  for (var i = 0; i < n; ++i)
    a[i] = i * 2;
  return a;
}

// The same growth through push, which reallocates the storage as it goes.
function pushLoop(n) {
  var a = [];
  for (var i = 0; i < n; ++i)
    a.push(i);
  return a.length;
}

// Reads with an index of every shape: integral, integral-valued double,
// fractional, negative, past the end, past 2^31 and past 2^32, and NaN. On an
// ordinary array all of those are just property lookups, and everything that
// is not an in-bounds index reads back undefined.
function idx(a, i) {
  return a[i];
}

// Allocate enough literals to force young-generation collections with some of
// them still live, so a collection has to scan arrays the emitted code built.
function churn(iters) {
  var keep = null;
  for (var i = 0; i < iters; ++i) {
    var a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
    if ((i & 4095) === 0)
      keep = a;
  }
  return keep === null ? -1 : keep.length;
}

// The whole file, `global` included, runs as emitted code now.
// CHECK: JIT successfully compiled FunctionID 0, 'global'
// CHECK0: JIT successfully compiled FunctionID 0, 'global'
var s = small();
// CHECK: JIT successfully compiled FunctionID 1, 'small'
// CHECK0: JIT successfully compiled FunctionID 1, 'small'
print(s.join(","), s.length);
// CHECK: 1,2,3 3
// CHECK0: 1,2,3 3

var k = kinds();
// CHECK: JIT successfully compiled FunctionID 2, 'kinds'
// CHECK0: JIT successfully compiled FunctionID 2, 'kinds'
print(k.join("|"), k.length, k[3] === null);
// CHECK: 1|two|true||3.5|-0.25 6 true
// CHECK0: 1|two|true||3.5|-0.25 6 true

var b = big();
// CHECK: JIT successfully compiled FunctionID 3, 'big'
// CHECK0: JIT successfully compiled FunctionID 3, 'big'
print(b.length, b[0], b[19], b[39], sum(b));
// CHECK: JIT successfully compiled FunctionID 8, 'sum'
// CHECK0: JIT successfully compiled FunctionID 8, 'sum'
// CHECK: 40 0 19 39 780
// CHECK0: 40 0 19 39 780

var c = computed(7);
// CHECK: JIT successfully compiled FunctionID 4, 'computed'
// CHECK0: JIT successfully compiled FunctionID 4, 'computed'
print(c.join(","), c.length);
// CHECK: 1,7,3 3
// CHECK0: 1,7,3 3

var h = holes();
// CHECK: JIT successfully compiled FunctionID 5, 'holes'
// CHECK0: JIT successfully compiled FunctionID 5, 'holes'
print(h.length, h[1] === undefined, 1 in h);
// CHECK: 3 true false
// CHECK0: 3 true false

var e = empty();
// CHECK: JIT successfully compiled FunctionID 6, 'empty'
// CHECK0: JIT successfully compiled FunctionID 6, 'empty'
print(e.length, Array.isArray(e));
// CHECK: 0 true
// CHECK0: 0 true

var sp = spread(s);
// CHECK: JIT successfully compiled FunctionID 7, 'spread'
// CHECK0: JIT successfully compiled FunctionID 7, 'spread'
print(sp.join(","), sp.length);
// CHECK: 1,2,3,99 4
// CHECK0: 1,2,3,99 4

var f = fill(10);
// CHECK: JIT successfully compiled FunctionID 9, 'fill'
// CHECK0: JIT successfully compiled FunctionID 9, 'fill'
print(f.join(","), f.length, sum(f));
// CHECK: 0,2,4,6,8,10,12,14,16,18 10 90
// CHECK0: 0,2,4,6,8,10,12,14,16,18 10 90

print(pushLoop(1000));
// CHECK: JIT successfully compiled FunctionID 10, 'pushLoop'
// CHECK0: JIT successfully compiled FunctionID 10, 'pushLoop'
// CHECK: 1000
// CHECK0: 1000

print(idx(b, 0), idx(b, 5.0), idx(b, 39));
// CHECK: JIT successfully compiled FunctionID 11, 'idx'
// CHECK0: JIT successfully compiled FunctionID 11, 'idx'
// CHECK: 0 5 39
// CHECK0: 0 5 39
print(idx(b, 2.5), idx(b, -1), idx(b, 40));
// CHECK-NEXT: undefined undefined undefined
// CHECK0-NEXT: undefined undefined undefined
print(idx(b, 2147483648), idx(b, 4294967296), idx(b, NaN));
// CHECK-NEXT: undefined undefined undefined
// CHECK0-NEXT: undefined undefined undefined

print(churn(30000));
// CHECK: JIT successfully compiled FunctionID 12, 'churn'
// CHECK0: JIT successfully compiled FunctionID 12, 'churn'
// CHECK: 12
// CHECK0: 12

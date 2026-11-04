/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed -fno-inline %s > %t.int && %hermes -typed -fno-inline -Xjit=force -Xjit-crash-on-error %s > %t.jit && diff %t.int %t.jit
// RUN: %hermes -typed -fno-inline %s > %t.int && %hermes -typed -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit2 && diff %t.int %t.jit2
// RUN: %hermes -typed -fno-inline -O0 %s > %t.int0 && %hermes -typed -fno-inline -O0 -Xjit=force -Xjit-crash-on-error %s > %t.jit0 && diff %t.int0 %t.jit0
// RUN: %hermes -typed -fno-inline -O0 %s > %t.int0 && %hermes -typed -fno-inline -O0 -Xjit=force -Xjit-crash-on-error -Xjit-emit-type-asserts %s > %t.jit3 && diff %t.int0 %t.jit3
// RUN: %hermes -typed -fno-inline -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -fno-inline -O0 -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines --check-prefix=CHECK0 %s
// REQUIRES: jit

// The FastArray family: NewFastArray, FastArrayLength, FastArrayLoad,
// FastArrayStore, FastArrayPush and FastArrayAppend. Only fastArrayLoad has
// an inline fast path; the rest are runtime calls, and fastArrayLength is a
// single load out of the object.
//
// These opcodes exist only in typed code, so every RUN line passes -typed.
// That is also why this is a separate file from arrays.js: in typed mode
// *every* array literal is a FastArray, including an untyped one and one
// annotated `any`, so NewArrayWithBuffer is unreachable here and the
// FastArray family is unreachable there.
//
// -fno-inline is what keeps the functions below separate. Without it the
// optimizer inlines all of them into the module's top-level function, which
// still compiles and still runs every opcode, but leaves nothing to pin per
// emitter. With it, each function is compiled and pinned on its own.
//
// An out-of-bounds FastArrayLoad throws, unlike an ordinary array's
// undefined, so the index cases go through catchLd. Every function here now
// compiles, including the catch helpers -- catchInst landed with the
// exceptions milestone -- which is why the four differential RUN lines carry
// -Xjit-crash-on-error. In catchLd the load happens one frame down, inside
// `ld`, so the throw crosses a compiled frame boundary.
//
// `tryLoad` is the case where the load is INSIDE the try region itself, so
// fastArrayLoad's isInTry() branch runs: it syncs the frame registers to the
// memory frame without freeing them, because the catch handler may read them
// again after the longjmp. tryLoad's handler does exactly that -- it uses
// both `a` and `i` -- so a missing sync shows up as a wrong answer. Before
// the exceptions milestone that branch was unreachable, since a function
// with an exception table declined.
//
// The index cases are what exercise emit_double_is_uint32, whose x86 form
// differs from arm64's fcvtzu (see its comment in JitEmitter-internal.h):
//  - -0 must load element 0, not throw: it is the one negative input that
//    round-trips to a valid index.
//  - 2^31 is a valid uint32 that is out of bounds. A signed comparison would
//    read it as negative and let it through.
//  - 2^32 is not a uint32 at all, and 2.5 is not an integer.
//  - NaN converts to the "integer indefinite" value, whose low 32 bits are
//    zero. Without the extra unordered test it would read element 0 -- x86's
//    vucomisd reports unordered as equal, where arm64's fcmp reports it as
//    not-equal.
//  - the index equal to the length is the bounds check's boundary; one off
//    there reads a slot past the storage.

function lit(): Array<number> {
  return [10, 20, 30];
}

// NewFastArray with a zero size hint, then growth through push.
function mk(n: number): Array<number> {
  var a: Array<number> = [];
  for (let i = 0; i < n; ++i)
    a.push(i * 2);
  return a;
}

function len(a: Array<number>): number {
  return a.length;
}

function ld(a: Array<number>, i: number): number {
  return a[i];
}

function st(a: Array<number>, i: number, v: number): void {
  a[i] = v;
}

// FastArrayLoad and FastArrayLength in a loop, every index in bounds.
function sum(a: Array<number>): number {
  var s: number = 0;
  for (let i = 0; i < a.length; ++i)
    s = s + a[i];
  return s;
}

// Load and store to the same array in a loop.
function dbl(a: Array<number>): void {
  for (let i = 0; i < a.length; ++i)
    a[i] = a[i] * 2;
}

// A spread of two arrays: NewFastArray followed by two FastArrayAppends.
function cat(a: Array<number>, b: Array<number>): Array<number> {
  return [...a, ...b];
}

function catchLd(a: Array<number>, i: number): any {
  try {
    return ld(a, i);
  } catch (e) {
    return "oob";
  }
}

function catchSt(a: Array<number>, i: number, v: number): any {
  try {
    st(a, i, v);
    return "ok";
  } catch (e) {
    return "oob";
  }
}

// The load itself is inside the try, and the handler reads both `a` and `i`
// back out of the frame after the longjmp.
function tryLoad(a: Array<number>, i: number, j: number): number {
  try {
    return a[i] + a[j];
  } catch (e) {
    return a.length * 100 + i;
  }
}

// Allocate fast arrays in a loop with a few of them kept live, so a young
// generation collection has to scan arrays and storages the emitted code
// produced.
function churn(iters: number): number {
  var keep: Array<number> = [];
  for (let i = 0; i < iters; ++i) {
    var a: Array<number> = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
    if (i % 4096 === 0)
      keep = a;
  }
  return keep.length;
}

var q: Array<number> = lit();
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'lit'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'lit'
print(q[0], q[1], q[2]);
// CHECK: 10 20 30
// CHECK0: 10 20 30

// With -fno-inline, `a.length` on a typed array is a call into the typed
// prelude's own `length` function, and that is where FastArrayLength lives --
// hence the second status line, which is the emitter's real pin for it.
print(len(q));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'len'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'len'
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'length'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'length'
// CHECK: 3
// CHECK0: 3

print(ld(q, 0), ld(q, 1), ld(q, 2), ld(q, 2.0));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'ld'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'ld'
// CHECK: 10 20 30 30
// CHECK0: 10 20 30 30

st(q, 1, 99);
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'st'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'st'
print(ld(q, 1));
// CHECK: 99
// CHECK0: 99

print(sum(q));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'sum'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'sum'
// CHECK: 139
// CHECK0: 139

dbl(q);
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'dbl'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'dbl'
print(sum(q), ld(q, 0), ld(q, 2));
// CHECK: 278 20 60
// CHECK0: 278 20 60

var m: Array<number> = mk(1000);
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'mk'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'mk'
print(len(m), ld(m, 0), ld(m, 999), sum(m));
// CHECK: 1000 0 1998 999000
// CHECK0: 1000 0 1998 999000

var c: Array<number> = cat(q, m);
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'cat'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'cat'
print(len(c), ld(c, 0), ld(c, 2), ld(c, 3), ld(c, 1002));
// CHECK: 1003 20 60 0 1998
// CHECK0: 1003 20 60 0 1998

// In bounds, including the last element and the -0 case.
print(catchLd(q, 0), catchLd(q, 2), catchLd(q, -0));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'catchLd'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'catchLd'
// CHECK: 20 60 20
// CHECK0: 20 60 20
// The bounds-check boundary and everything that is not a uint32 index.
print(catchLd(q, 3), catchLd(q, 2.5), catchLd(q, -1));
// CHECK-NEXT: oob oob oob
// CHECK0-NEXT: oob oob oob
print(catchLd(q, 2147483648), catchLd(q, 4294967296));
// CHECK-NEXT: oob oob
// CHECK0-NEXT: oob oob
print(catchLd(q, NaN), catchLd(q, Infinity), catchLd(q, -Infinity));
// CHECK-NEXT: oob oob oob
// CHECK0-NEXT: oob oob oob

// The store path bounds-checks in the runtime, not in emitted code, but the
// same index shapes must give the same answers.
print(
  catchSt(q, 0, 1), catchSt(q, 3, 1), catchSt(q, 2.5, 1), catchSt(q, -1, 1));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'catchSt'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'catchSt'
// CHECK: ok oob oob oob
// CHECK0: ok oob oob oob
print(ld(q, 0));
// CHECK-NEXT: 1
// CHECK0-NEXT: 1

// In bounds, then each of the two loads out of bounds in turn. The handler's
// answer is a.length * 100 + i, so it depends on both frame registers.
print(tryLoad(q, 0, 2), tryLoad(q, 5, 2), tryLoad(q, 0, 7));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'tryLoad'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'tryLoad'
// CHECK-NEXT: 61 305 300
// CHECK0-NEXT: 61 305 300

print(churn(30000));
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'churn'
// CHECK0: JIT successfully compiled FunctionID {{[0-9]+}}, 'churn'
// CHECK: 12
// CHECK0: 12

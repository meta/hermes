/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -verify-ir -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --match-full-lines %s

// Recursive type aliases whose cycle runs through an array or a function type.
// Before they were lowered with cycle breaking, compiling any of them
// overflowed the stack.

(function () {

// Cycle through an array.
type Json = number | string | Json[];
function json(j: Json): Json {
  return j;
}
const inner: Json[] = [2];
const outer: Json[] = [1, 'a', inner];
print(typeof json(1), typeof json('s'), typeof json(outer));
// CHECK: number string object

// Cycle through a function's return type.
type F = (x: number) => F;
function fself(x: number): F {
  return fself;
}
print(typeof fself(1)(2)(3));
// CHECK-NEXT: function

// The same, through a union.
type G = (x: number) => G | null;
function gnull(x: number): G | null {
  return x > 0 ? gnull : null;
}
print(gnull(0), typeof gnull(1));
// CHECK-NEXT: null function

// Cycle through a function's parameter and an array.
type FA = (x: FA[]) => number;
function count(x: FA[]): number {
  return x.length;
}
function callFA(f: FA): number {
  return f([f, f]);
}
print(callFA(count));
// CHECK-NEXT: 2

// Mutual recursion between an array alias and a function alias.
type A = B[];
type B = (a: A) => B;
function bself(a: A): B {
  return bself;
}
print(typeof bself([bself]));
// CHECK-NEXT: function

// A class whose field is an array of functions returning the class.
class K {
  fs: (() => K)[];
  constructor() {
    this.fs = [];
  }
}
const k = new K();
k.fs.push(() => k);
print(k.fs.length, typeof k.fs[0]().fs);
// CHECK-NEXT: 1 object

// Identity: every Flow type equal to a recursive alias must lower to the same
// IR type, or storing a value of one into a slot of the other fails IR
// verification. Static fields are typed frame slots, and `local` is a typed
// variable captured by a closure.
class Store {
  static j: Json;
  static f: F;
}
function makeJ(): Json {
  return outer;
}
function makeF(): F {
  return fself;
}
function keepJ(j: Json): Json {
  let local: Json = j;
  function read(): Json {
    return local;
  }
  Store.j = local;
  return read();
}
function keepF(f: F): F {
  let local: F = f;
  function read(): F {
    return local;
  }
  Store.f = local;
  return read();
}
print(typeof keepJ(makeJ()), typeof Store.j);
// CHECK-NEXT: object object
print(typeof keepF(makeF())(1), typeof Store.f);
// CHECK-NEXT: function function

// The declared signature of `fself`, `(x: number) => F`, is a different Flow
// type object than `F`, but Flow considers them equal.
Store.f = fself;
print(typeof Store.f(1));
// CHECK-NEXT: function

// A second alias equal to F.
type F2 = (x: number) => F;
function keepF2(f2: F2): F {
  Store.f = f2;
  return f2;
}
print(typeof keepF2(fself)(1));
// CHECK-NEXT: function

// Two mutually recursive aliases that Flow considers equal to each other.
type P = (x: number) => Q;
type Q = (x: number) => P;
class StorePQ {
  static p: P;
  static q: Q;
}
function pq(x: number): Q {
  return pq;
}
function keepPQ(q: Q): P {
  StorePQ.p = q;
  StorePQ.q = q;
  return q;
}
print(typeof keepPQ(pq)(1)(2));
// CHECK-NEXT: function

})();

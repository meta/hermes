/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 -typed %s | %FileCheck %s --match-full-lines
// RUN: %hermes -O -fno-inline -typed %s | %FileCheck %s --match-full-lines
// RUN: %shermes -O0 -typed -exec %s | %FileCheck %s --match-full-lines
// RUN: %shermes -O -fno-inline -typed -exec %s | %FileCheck %s --match-full-lines

// Every reference to one specialization of a generic class lowers to the same
// IR class, and a subclass of a specialization is an IR subclass of it. If
// two operands of a === below lowered to unrelated IR classes, -O would fold
// the comparison to false.

class A<T> {
  v: T;
  constructor(v: T) {
    this.v = v;
  }
}
class B2<T> extends A<T> {
  constructor(v: T) {
    super(v);
  }
}
class B3<T> extends A<number> {
  w: T;
  constructor(v: number, w: T) {
    super(v);
    this.w = w;
  }
}
class Node<T> {
  next: Node<T> | null;
  constructor() {
    this.next = null;
  }
}
type W<T> = A<T>;

function make<U>(u: U): A<U> {
  return new A<U>(u);
}

function sameAlias(x: A<number>, y: W<number>): boolean {
  return x === y;
}
function sameSuper(x: A<string>, y: B2<string>): boolean {
  return x === y;
}
function fixedSuper(x: A<number>, y: B3<string>): boolean {
  return x === y;
}
function selfLoop(n: Node<number>): boolean {
  return n.next === n;
}

// Keep the parameter types: unknown callers.
globalThis.f = [sameAlias, sameSuper, fixedSuper, selfLoop];

const a: A<number> = make<number>(1);
const b2: B2<string> = new B2<string>('s');
const b3: B3<string> = new B3<string>(2, 'w');
const n: Node<number> = new Node<number>();
n.next = n;
print(sameAlias(a, a), sameSuper(b2, b2), fixedSuper(b3, b3), selfLoop(n));
// CHECK: true true true true

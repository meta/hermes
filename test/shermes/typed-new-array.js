/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -O0 -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -typed -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -typed -verify-ir %s | %FileCheck --match-full-lines %s

// `new Array<T>()` and `new` of a class deriving from Array<T> allocate fast
// arrays, which the fast-array builtins (push, length, indexing) require.
// Methods of a subclass live on its home object, so a field-less subclass
// instance can be a fast array.

'use strict';

class Foo extends Array<number> {
  // Calls the inherited `length` getter on a fast-array `this`.
  sum(): number {
    let s: number = 0;
    const a: number[] = this;
    for (let i: number = 0; i < this.length; ++i) s += a[i];
    return s;
  }
}

// An explicit constructor whose super() call reaches Array's implicit
// constructor.
class WithCtor extends Array<number> {
  constructor(n: number) {
    super();
    const a: number[] = this;
    for (let i: number = 0; i < n; ++i) a.push(i * 10);
  }
  first(): number {
    const a: number[] = this;
    return a[0];
  }
}

// An implicit constructor forwarding to WithCtor's, and a method call that
// looks up an inherited method on the prototype chain.
class Derived extends WithCtor {
  firstPlusLength(): number {
    return this.first() + this.length;
  }
}

function main(): void {
  const c: number[] = new Array<number>();
  c.push(1);
  c.push(2);
  print(c.length, c[0], c[1]);
  // CHECK: 2 1 2

  const f: Foo = new Foo();
  const a: number[] = f;
  a.push(3);
  a.push(4);
  print(f.sum(), a.length, f.length);
  // CHECK-NEXT: 7 2 2

  const d: Derived = new Derived(3);
  print(d.firstPlusLength(), d.length, d instanceof WithCtor);
  // CHECK-NEXT: 3 3 true
}

main();

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed %s | %FileCheck %s --match-full-lines
// RUN: %shermes -typed -exec %s | %FileCheck %s --match-full-lines

// Function declarations nested in generic specializations are hoisted.

function f<T>(x: T, y: number): number {
  const r = g();
  function g(): number {
    return y;
  }
  return r;
}
print(f<number>(1, 3), f<string>("a", 4));
// CHECK: 3 4

class C<T> {
  v: T;
  constructor(v: T) {
    this.v = v;
  }
  m(): T {
    return h(this.v);
    function h(x: T): T {
      return x;
    }
  }
}
print(new C<number>(5).m(), new C<string>("s").m());
// CHECK-NEXT: 5 s

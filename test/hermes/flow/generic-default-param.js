/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed %s | %FileCheck %s --match-full-lines
// RUN: %shermes -typed -exec %s | %FileCheck %s --match-full-lines

// Default parameter values in generic specializations, which put the
// parameters in a separate scope.

function f<T>(x: T, y: number = 2): number {
  return arguments.length + y;
}
print(f<number>(1), f<number>(1, 5));
// CHECK: 3 7

function g<T>(x: T, y: () => T = (): T => x): T {
  return y();
}
print(g<number>(3), g<string>("s", (): string => "t"));
// CHECK-NEXT: 3 t

function h<T>(x: T): number {
  const k = (y: number = 1): number => y + 1;
  return k();
}
print(h<number>(1));
// CHECK-NEXT: 2

class C<T> {
  v: T;
  constructor(v: T) {
    this.v = v;
  }
  m(n: number = 10): number {
    return n;
  }
}
print(new C<number>(1).m(), new C<string>("a").m(4));
// CHECK-NEXT: 10 4

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed %s | %FileCheck %s --match-full-lines
// RUN: %shermes -typed -exec %s | %FileCheck %s --match-full-lines

// Named function expressions in generic specializations.

function f<T>(x: T, n: number): number {
  const fact = function g(k: number): number {
    return k <= 1 ? 1 : k * g(k - 1);
  };
  return fact(n);
}
print(f<number>(1, 4), f<string>("a", 3));
// CHECK: 24 6

function h<T>(x: T, cb: () => T = function g(): T { return x; }): T {
  return cb();
}
print(h<number>(7), h<string>("s"));
// CHECK-NEXT: 7 s

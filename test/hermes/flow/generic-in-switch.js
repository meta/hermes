/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed %s | %FileCheck %s --match-full-lines
// RUN: %shermes -typed -exec %s | %FileCheck %s --match-full-lines

// Generics declared directly in a switch case.

function run(k: number): void {
  switch (k) {
    case 1:
      class C<T> {
        x: T;
        constructor(x: T) {
          this.x = x;
        }
      }
      print(new C<number>(5).x);
    case 2:
      function g<T>(a: T): T {
        return a;
      }
      type A<T> = T;
      const s: A<string> = g<string>("s");
      print(s);
  }
}
run(1);
// CHECK: 5
// CHECK-NEXT: s
run(2);
// CHECK-NEXT: s

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -typed --dump-sema %s 2>&1 ) | %FileCheck %s --match-full-lines

// Inference with type params from another scope gives up on non-identifier
// params.

class A {
  @Hermes.final m<T>(x: T, y: number = 1): T { return x; }
  @Hermes.final r<T>(...xs: T[]): void {}
}
let n: number = new A().m(1);
// CHECK: {{.*}}:[[@LINE-1]]:17: error: ft: could not infer type arguments for generic method
new A().r(1);
// CHECK: {{.*}}:[[@LINE-1]]:1: error: ft: could not infer type arguments for generic method

class B<T, U> {
  constructor(x: U, y: number = 1) {}
}
new B<number>("s");
// CHECK: {{.*}}:[[@LINE-1]]:6: error: ft: could not infer type argument for type parameter 'U'

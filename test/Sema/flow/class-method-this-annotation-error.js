/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -typed --dump-sema %s 2>&1 ) | %FileCheck %s --match-full-lines

// Mismatched 'this' annotations on methods are reported at the 'this' param.

function f1(): void {
  class D {}
  class C {
    foo(this: number): void {}
// CHECK: {{.*}}:[[@LINE-1]]:9: error: ft: incompatible 'this' type annotation
    static s(this: number): void {}
// CHECK: {{.*}}:[[@LINE-1]]:14: error: ft: incompatible 'this' type annotation
    constructor(this: number) {}
// CHECK: {{.*}}:[[@LINE-1]]:17: error: ft: incompatible 'this' type annotation
    bar(this: D): void {}
// CHECK: {{.*}}:[[@LINE-1]]:9: error: ft: incompatible 'this' type annotation
  }
}

// A type argument may be a shared type with no node.
function f2(): void {
  class G<T> {
    m(this: T): void {}
// CHECK: {{.*}}:[[@LINE-1]]:7: error: ft: incompatible 'this' type annotation
  }
  new G<string>();
}

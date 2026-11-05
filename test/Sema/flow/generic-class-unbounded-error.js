/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -typed --dump-sema %s 2>&1 ) | %FileCheck %s --match-full-lines

// Generic class specializations that keep creating new ones are cut off.

function f1(): void {
  class A<T> {
    m(): void {
      new A<[T]>();
// CHECK: {{.*}}:[[@LINE-1]]:12: error: ft: too deeply nested generic class specialization
    }
  }
  new A<number>();
}

function f2(): void {
  class B<T> {
    x: ?B<[T]> = null;
// CHECK: {{.*}}:[[@LINE-1]]:10: error: ft: too deeply nested generic class specialization
  }
  const b: B<number> = new B<number>();
}

function f3(): void {
  class C<T> extends D<[T]> {}
  class D<T> {
    m(): void {
      new C<T>();
// CHECK: {{.*}}:[[@LINE-1]]:12: error: ft: too deeply nested generic class specialization
    }
  }
  new C<number>();
}

function f4(): void {
  class E<T: mixed | E<[T]>> {}
// CHECK: {{.*}}:[[@LINE-1]]:23: error: ft: too deeply nested generic class specialization
  type X = E<number>;
}

function f5(): void {
  type G<T: mixed | G<{f: T}>> = T;
  type X = G<number>;
// CHECK: {{.*}}:[[@LINE-2]]:21: error: ft: too deeply nested generic type alias expansion
}

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -typed --dump-sema %s 2>&1 ) | %FileCheck %s --match-full-lines

// A wrong type argument count is reported on every use.

function f<T>(x: T): T {
  return x;
}
f<number, string>(1);
// CHECK: {{.*}}:[[@LINE-1]]:2: error: type argument mismatch, expected 1, found 2
f<number, string>(1);
// CHECK: {{.*}}:[[@LINE-1]]:2: error: type argument mismatch, expected 1, found 2

class C<T> {}
new C<number, string>();
// CHECK: {{.*}}:[[@LINE-1]]:6: error: type argument mismatch, expected 1, found 2
new C<number, string>();
// CHECK: {{.*}}:[[@LINE-1]]:6: error: type argument mismatch, expected 1, found 2

class M {
  @Hermes.final m<T>(x: T): T {
    return x;
  }
}
new M().m<number, string>(1);
// CHECK: {{.*}}:[[@LINE-1]]:1: error: type argument mismatch, expected 1, found 2
new M().m<number, string>(1);
// CHECK: {{.*}}:[[@LINE-1]]:1: error: type argument mismatch, expected 1, found 2

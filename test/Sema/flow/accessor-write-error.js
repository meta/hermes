/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -typed -dump-sema -ferror-limit=0 %s 2>&1 ) | %FileCheck --match-full-lines %s

// Accessors live in Variables, not in the instance class, so a write that
// escapes sema reaches IRGen and either asserts or (for statics) silently
// stores a plain own property that bypasses the accessor. Only `=` and
// compound assignment used to be rejected, so every other write form is
// covered here.

class A {
  @Hermes.final get g(): number {
    return 1;
  }
  @Hermes.final set s(v: number) {}
  @Hermes.final static get sg(): number {
    return 2;
  }
  @Hermes.final static set ss(v: number) {}
}

let a = new A();

// Every write form against a getter-only accessor.
a.g = 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property g
[a.g] = [1];
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:4: error: ft: cannot assign to readonly property g
({p: a.g} = {p: 1});
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:8: error: ft: cannot assign to readonly property g
for (a.g in {}) {}
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:8: error: ft: cannot assign to readonly property g
for (a.g of [1]) {}
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:8: error: ft: cannot assign to readonly property g
a.g += 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property g
a.g++;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property g
a.g ||= 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property g
a.g &&= 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property g
a.g ??= 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property g

// Statics reach a different IRGen path: the write became a plain
// StorePropertyStrict on the class object rather than a setter call.
A.sg = 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property sg
A.sg++;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property sg
A.sg ??= 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot assign to readonly property sg

// A setter-only accessor rejects reads, including the read half of a
// compound assignment, of a logical assignment, and of ++.
let r: number = a.s;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:19: error: ft: cannot read writeonly property s
a.s += 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot read writeonly property s
a.s++;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot read writeonly property s
a.s ||= 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot read writeonly property s
let sr: number = A.ss;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:20: error: ft: cannot read writeonly property ss
A.ss ??= 1;
// CHECK: {{.*}}accessor-write-error.js:[[@LINE-1]]:3: error: ft: cannot read writeonly property ss

// A plain `=` to a setter-only accessor is a pure write, so it is allowed.
a.s = 1;
A.ss = 1;

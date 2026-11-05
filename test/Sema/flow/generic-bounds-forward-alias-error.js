/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes --typed --dump-sema %s 2>&1 ) | %FileCheckOrRegen %s --match-full-lines

// Generic class bounds referring to type aliases declared after the class,
// specialized while the aliases are still being resolved.

class A<T: ?U> {}
type U = number[];
type XA = A<number>;

class B<T: ?V> {}
type V = string | B<number>;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}generic-bounds-forward-alias-error.js:15:11: error: ft: type argument for type parameter 'T' is incompatible with its bound
// CHECK-NEXT:type XA = A<number>;
// CHECK-NEXT:          ^~~~~~~~~
// CHECK-NEXT:{{.*}}generic-bounds-forward-alias-error.js:18:19: error: ft: type argument for type parameter 'T' is incompatible with its bound
// CHECK-NEXT:type V = string | B<number>;
// CHECK-NEXT:                  ^~~~~~~~~
// CHECK-NEXT:Emitted 2 errors. exiting.

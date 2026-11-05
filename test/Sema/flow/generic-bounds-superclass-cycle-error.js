/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -typed -dump-sema %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

// A bound that reaches its own generic through a superclass.

class Base {}
class A<T: B<Base>> {}
class B<T> extends A<T> {}
let x: A<Base>;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}generic-bounds-superclass-cycle-error.js:15:9: error: ft: type argument for type parameter 'T' is incompatible with its bound
// CHECK-NEXT:let x: A<Base>;
// CHECK-NEXT:        ^~~~~~
// CHECK-NEXT:Emitted 1 errors. exiting.

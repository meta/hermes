/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -typed -dump-sema %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

// A generic class whose bound is a closed instantiation of itself. The bound is
// resolved once the specialization exists, and must not be silently accepted
// (which would let any argument satisfy the bound).

class Base {}
class Node<T extends Node<Base>> extends Base {}

let bad: Node<Base>;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}generic-bounds-recursive-class-error.js:17:14: error: ft: type argument for type parameter 'T' is incompatible with its bound
// CHECK-NEXT:let bad: Node<Base>;
// CHECK-NEXT:             ^~~~~~
// CHECK-NEXT:Emitted 1 errors. exiting.

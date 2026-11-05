/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -typed -dump-sema %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

class Base {}

type Alias<T> = Node<T>;

class Node<T: Alias<Base>> {}

// The bound Alias<Base> is the Node<Base> specialization itself.
// The bound must not silently become any and accept Base.
let bad: Node<Base>;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}generic-bounds-indirect-recursive-same-specialization-error.js:18:14: error: ft: type argument for type parameter 'T' is incompatible with its bound
// CHECK-NEXT:let bad: Node<Base>;
// CHECK-NEXT:             ^~~~~~
// CHECK-NEXT:Emitted 1 errors. exiting.

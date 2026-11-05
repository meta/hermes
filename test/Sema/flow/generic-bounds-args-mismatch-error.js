/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -typed -dump-sema %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

class Base {}
class Pair<A, B> {}
// Pair needs 2 arguments.
type Alias<T extends Pair<Base>> = T;
let z: Alias<Pair<Base, Base>>;

// The wrong-arity bound is resolved for two specializations.
class C<T extends Pair<Base>> {}
let x: C<number>;
let y: C<string>;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}generic-bounds-args-mismatch-error.js:13:26: error: type argument mismatch, expected 2, found 1
// CHECK-NEXT:type Alias<T extends Pair<Base>> = T;
// CHECK-NEXT:                         ^~~~~~
// CHECK-NEXT:{{.*}}generic-bounds-args-mismatch-error.js:17:23: error: type argument mismatch, expected 2, found 1
// CHECK-NEXT:class C<T extends Pair<Base>> {}
// CHECK-NEXT:                      ^~~~~~
// CHECK-NEXT:{{.*}}generic-bounds-args-mismatch-error.js:17:23: error: type argument mismatch, expected 2, found 1
// CHECK-NEXT:class C<T extends Pair<Base>> {}
// CHECK-NEXT:                      ^~~~~~
// CHECK-NEXT:Emitted 3 errors. exiting.

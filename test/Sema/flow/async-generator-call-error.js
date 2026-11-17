/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -typed -dump-sema %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

'use strict';

// A call to a typed async function or generator is 'mixed' in Flow: its value
// is a promise or a generator object, for which typed mode has no type. It
// must not flow implicitly into a concrete type. With 'any' it would, through
// a checked cast that for a class checks only that the value is an object, so
// a promise could be read as a class instance.

class Foo {
  x: number = 1;
}

async function af(): number {
  return 1;
}

function* g(): number {
  yield 1;
  return 2;
}

function f(): void {
  const c: Foo = af();
  const n: number = af();
  g().next();
  // Accepted: 'any' and 'mixed' destinations, and an explicit cast.
  const a: any = af();
  const m: mixed = g();
  (g(): any).next();
}

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}async-generator-call-error.js:32:9: error: ft: incompatible initialization type: cannot assign mixed to class Foo
// CHECK-NEXT:  const c: Foo = af();
// CHECK-NEXT:        ^~~~~~~~~~~~~
// CHECK-NEXT:{{.*}}async-generator-call-error.js:33:9: error: ft: incompatible initialization type: cannot assign mixed to number
// CHECK-NEXT:  const n: number = af();
// CHECK-NEXT:        ^~~~~~~~~~~~~~~~
// CHECK-NEXT:{{.*}}async-generator-call-error.js:34:7: error: ft: named property access only allowed on objects, found mixed
// CHECK-NEXT:  g().next();
// CHECK-NEXT:      ^~~~
// CHECK-NEXT:Emitted 3 errors. exiting.

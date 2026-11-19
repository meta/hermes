/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -typed -dump-sema -ferror-limit=0 %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

(function() {
  function bad(n: number, s: string, u: number|string) {
    n ??= s;
    n ||= s;
    n &&= s;
    // A union with more than one non-nullish arm can't be narrowed to the
    // target, same as for '='.
    n ??= u;
  }

  function badLength(a: number[]) {
    a.length ??= 1;
  }
})

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}logical-assign-error.js:12:5: error: ft: incompatible assignment type: cannot implicitly cast from string to number
// CHECK-NEXT:    n ??= s;
// CHECK-NEXT:    ^~~~~~~
// CHECK-NEXT:{{.*}}logical-assign-error.js:13:5: error: ft: incompatible assignment type: cannot implicitly cast from string to number
// CHECK-NEXT:    n ||= s;
// CHECK-NEXT:    ^~~~~~~
// CHECK-NEXT:{{.*}}logical-assign-error.js:14:5: error: ft: incompatible assignment type: cannot implicitly cast from string to number
// CHECK-NEXT:    n &&= s;
// CHECK-NEXT:    ^~~~~~~
// CHECK-NEXT:{{.*}}logical-assign-error.js:17:5: error: ft: incompatible assignment type: cannot implicitly cast from (string | number) to number
// CHECK-NEXT:    n ??= u;
// CHECK-NEXT:    ^~~~~~~
// CHECK-NEXT:{{.*}}logical-assign-error.js:21:5: error: ft: cannot assign to property 'length' of typed Array
// CHECK-NEXT:    a.length ??= 1;
// CHECK-NEXT:    ^~~~~~~~~~~~~~
// CHECK-NEXT:{{.*}}logical-assign-error.js:21:7: error: ft: cannot assign to readonly property length
// CHECK-NEXT:    a.length ??= 1;
// CHECK-NEXT:      ^~~~~~
// CHECK-NEXT:Emitted 6 errors. exiting.

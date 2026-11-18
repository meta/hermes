/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -fno-std-globals --typed --dump-sema -ferror-limit=0 %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

const o: {x: string, y: string} = {x: 'a', y: 'b'};

// A writable indexer could delete or retype a named field.
const d: {[string]: string} = o;

// A read-only indexer is fine, but can't delete through it.
const r: {+[string]: string} = o;
delete r.x;

// A literal is a fresh dictionary.
const l: {[string]: string} = {x: 'a'};
delete l.x;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}object-indexer-named-error.js:13:7: error: ft: incompatible initialization type: cannot assign object to object
// CHECK-NEXT:const d: {[string]: string} = o;
// CHECK-NEXT:      ^~~~~~~~~~~~~~~~~~~~~~~~~
// CHECK-NEXT:{{.*}}object-indexer-named-error.js:17:10: error: ft: cannot assign to readonly indexer
// CHECK-NEXT:delete r.x;
// CHECK-NEXT:         ^
// CHECK-NEXT:Emitted 2 errors. exiting.

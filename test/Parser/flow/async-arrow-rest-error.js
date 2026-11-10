/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc -parse-flow -dump-ast -pretty-json %s 2>&1 ) | %FileCheckOrRegen --match-full-lines %s

c ? async (...a.b) => 0 : 1;

// Auto-generated content below. Please do not modify manually.

// CHECK:{{.*}}async-arrow-rest-error.js:10:15: error: identifier or pattern expected
// CHECK-NEXT:c ? async (...a.b) => 0 : 1;
// CHECK-NEXT:              ^~~
// CHECK-NEXT:Emitted 1 errors. exiting.

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %hermesc -dump-transformed-ast %s 2>&1 ) | %FileCheck %s

print(/[b-a]/);
//CHECK: {{.*}}regexp.js:10:7: error: Invalid regular expression: Character class range out of order
//CHECK-NEXT: print(/[b-a]/);
//CHECK-NEXT:       ^~~~~~~

print(/(abc/);
//CHECK: {{.*}}regexp.js:15:7: error: Invalid regular expression: Parenthesized expression not closed
//CHECK-NEXT: print(/(abc/);
//CHECK-NEXT:       ^~~~~~

print(/.{5,3}/);
//CHECK: {{.*}}regexp.js:20:7: error: Invalid regular expression: Quantifier range out of order
//CHECK-NEXT: print(/.{5,3}/);
//CHECK-NEXT:       ^~~~~~~~

print(/{100}/);
//CHECK: {{.*}}regexp.js:25:7: error: Invalid regular expression: Quantifier has nothing to repeat
//CHECK-NEXT: print(/{100}/);
//CHECK-NEXT:       ^~~~~~~

// Reusing a capture group name is only allowed when the two groups are in
// different alternatives, so this one is still an error.
print(/(?<a>x)(?<a>y)/);
//CHECK: {{.*}}regexp.js:32:7: error: Invalid regular expression: Duplicate capture group name
//CHECK-NEXT: print(/(?<a>x)(?<a>y)/);
//CHECK-NEXT:       ^~~~~~~~~~~~~~~~

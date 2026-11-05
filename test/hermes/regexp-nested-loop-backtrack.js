/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s

// Entering a counted loop again must not lose the iteration count of its
// previous entry, which is still needed when backtracking into that entry.

print('nested loop backtrack');
// CHECK-LABEL: nested loop backtrack

print(/(?:(?:a+){1}){2}/.test("aa"));
// CHECK-NEXT: true
print(/(?:(?:a+){1,3}){2}/.test("aa"));
// CHECK-NEXT: true
print(/^(?:(?:(?:\W+){1,1})*\s)$/.test(" \n"));
// CHECK-NEXT: true
print(/^(?:(?:(?:\W+){1,1})*\s)$/.test(String.fromCharCode(0x2028) + "\n"));
// CHECK-NEXT: true
print(/(?:(a+){1}){2}/.exec("aaa"));
// CHECK-NEXT: aaa,a
print(/^((?:(a+){1}){2})$/.exec("aaa"));
// CHECK-NEXT: aaa,aaa,a
print(/(?:(?:a+){1}){2}/.exec("baa"));
// CHECK-NEXT: aa
print(/^(?:(?:a+){1})+?a$/.exec("aa"));
// CHECK-NEXT: aa
print(/(?<=(?:(?:a+){1}){2})b/.exec("aab"));
// CHECK-NEXT: b
try {
  print(/(?:(?:a?){1}){1,3}c/.exec("a"));
} catch (e) {
  print(e.name);
}
// CHECK-NEXT: null
print(/(?:(?:a+){2}){2}/.test("aaa"));
// CHECK-NEXT: false

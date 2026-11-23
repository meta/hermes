/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

// Regression test: entering a loop again from an enclosing loop must not
// lose the iteration count of the earlier entry, which backtracking may
// resume.

print(JSON.stringify(/((a+)+)+a/.exec("aa")));
// CHECK: ["aa","a","a"]
print(/(?:(?:a+){1}){2}/.test("aa"));
// CHECK-NEXT: true
print(/^(?:(?:(?:\W+){1,1})*\s)$/.test("\u2028\n"));
// CHECK-NEXT: true

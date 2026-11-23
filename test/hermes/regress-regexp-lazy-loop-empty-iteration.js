/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

// Regression test: after backtracking into an iteration of a non-greedy
// loop, the empty-iteration check must compare against where that
// iteration started.

print(/(a?)*?b/.test("a"));
// CHECK: false
print(/^(?:(?:^a?)+?)$/.test("aa"));
// CHECK-NEXT: false
print(JSON.stringify(/^(?:(a)|a)*?\1$/.exec("a")));
// CHECK-NEXT: ["a",null]

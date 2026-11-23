/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s

// Regression test: inside a lookbehind, a ^ in one alternative must not
// prune it based on the position where it ends.

print(/(?<=^a|b)/.test("a"));
// CHECK: true
print(/a(?<=x|^a)/.test("a"));
// CHECK-NEXT: true

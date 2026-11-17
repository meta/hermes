/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -commonjs %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -commonjs %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -commonjs %s | %FileCheck --match-full-lines %s

// Regression test for #1990: compiling with -commonjs used to crash IRGen
// because doCJSModule ran without an active FunctionContext, and the
// optimizer used to delete CJS module functions as unused.

print('cjs module start');
// CHECK: cjs module start

print(typeof exports, typeof require, typeof module);
// CHECK-NEXT: object function object

exports.foo = 42;
print(module.exports.foo);
// CHECK-NEXT: 42

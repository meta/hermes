/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=2 -Xjit-crash-on-error %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

function retNumber() {
  this.x = 3;
  // Ensure 123 is ignored.
  return 123;
}

function retObject() {
  this.x = 10;
  // Ensure the new object is used.
  return {x: 45};
}

print("retNumber", new retNumber().x);
// CHECK: JIT successfully compiled FunctionID 1, 'retNumber'
// CHECK: retNumber 3
print("retObject", new retObject().x);
// CHECK: JIT successfully compiled FunctionID 2, 'retObject'
// CHECK: retObject 45

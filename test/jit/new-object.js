/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

function newObject() {
  return {};
}

function newObjectWithParent() {
  return {__proto__: Math};
}

function newObjectWithBuffer(c) {
  return {a: 1, b: 3, [c]: 40};
}

print("newObject", newObject());
// CHECK: JIT successfully compiled FunctionID 1, 'newObject'
// CHECK-NEXT: newObject [object Object]
print("newObjectWithParent", Object.getPrototypeOf(newObjectWithParent()));
// CHECK: JIT successfully compiled FunctionID 2, 'newObjectWithParent'
// CHECK: newObjectWithParent [object Math]
var o = newObjectWithBuffer('c');
print("newObjectWithBuffer", o.a, o.c);
// CHECK: JIT successfully compiled FunctionID 3, 'newObjectWithBuffer'
// CHECK-NEXT: newObjectWithBuffer 1 40

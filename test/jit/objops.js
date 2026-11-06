/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// GetGlobalObject
// PutByIdLoose
// TryGetById
function foo() {
    a = 10;
    b = 20;
    return a + b;
}

// PutByIdLoose
// PutByValLoose
// GetById
// GetByVal
function bar(o, p) {
    o.a = 1;
    o.b = 2;
    o[p] = 100;
    return o.a + o.b + o[p];
}

function isIn(a, prop) {
    return prop in a;
}

function getByIndex(o) {
    return o[1];
}

print("foo", foo());
// CHECK: JIT successfully compiled FunctionID 1, 'foo'
// CHECK: foo 30
print("bar", bar({}, "prop"));
// CHECK: JIT successfully compiled FunctionID 2, 'bar'
// CHECK: bar 103
print("isIn", isIn({}, "prop"));
// CHECK: JIT successfully compiled FunctionID 3, 'isIn'
// CHECK-NEXT: isIn false
print("isIn", isIn({prop: 1}, "prop"));
// CHECK-NEXT: isIn true
print("getByIndex", getByIndex(0));
// CHECK: JIT successfully compiled FunctionID 4, 'getByIndex'
// CHECK-NEXT: getByIndex undefined
print("getByIndex", getByIndex("abc"));
// CHECK-NEXT: getByIndex b
print("getByIndex", getByIndex([10, 20, 30]));
// CHECK-NEXT: getByIndex 20
print("getByIndex", getByIndex({0:11, 1: 21, 2: 31}));
// CHECK-NEXT: getByIndex 21

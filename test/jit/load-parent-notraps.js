/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

const obj1 = { prop: 1 };

const c1 = {
  __proto__: obj1,
  printSuperProp() {
    print(super.prop);
  },
};

c1.printSuperProp();

const c2 = {
  __proto__: null,
  printSuperProp() {
    print(super.prop);
  },
};

try {
    c2.printSuperProp();
} catch (e) {
    print(e.message);
}

// CHECK: JIT successfully compiled FunctionID 1, 'printSuperProp'
// CHECK: 1

// CHECK: JIT successfully compiled FunctionID 2, 'printSuperProp'
// CHECK: Cannot read property 'prop' of null

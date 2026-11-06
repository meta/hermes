/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

function constants(o) {
    o.zero = 0;
    o.uint8 = 5;
    o.int = 100_000_000;
    o.d = 3.14;
    o.undefined = void 0;
    o.null = null;
    o.true = true;
    o.false = false;
    o.str = "Hello";
    return o;
}

print(JSON.stringify(constants({}), undefined, 2));
// CHECK: JIT successfully compiled FunctionID 1, 'constants'
// CHECK: {
// CHECK-NEXT:   "zero": 0,
// CHECK-NEXT:   "uint8": 5,
// CHECK-NEXT:   "int": 100000000,
// CHECK-NEXT:   "d": 3.14,
// CHECK-NEXT:   "null": null,
// CHECK-NEXT:   "true": true,
// CHECK-NEXT:   "false": false,
// CHECK-NEXT:   "str": "Hello"
// CHECK-NEXT: }

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xjit=force -Xdump-jitcode=2 %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

let x = 10;
let y = closure;

function closure(z) {
  print(x, z, y);
  // CreateEnvironment
  return function inner() { return z; }
}

var f = closure(20);
// CHECK: JIT successfully compiled FunctionID 1, 'closure'
// CHECK: 10 20 function closure({{.*}}
print(f());
// CHECK: JIT successfully compiled FunctionID 2, 'inner'
// CHECK-NEXT: 20

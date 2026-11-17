/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=foo -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

return function foo(a: number[]): number {
  return a[0];
}

// Auto-generated content below. Please do not modify manually.

// CHECK:function foo(a: array(number)): number [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:array(number)) %a: array(number)
// CHECK-NEXT:  %1 = FastArrayLoadInst (:number) %0: array(number), 0: number
// CHECK-NEXT:       ReturnInst %1: number
// CHECK-NEXT:function_end

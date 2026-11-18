/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=f -typed -dump-lir -O %s | %FileCheckOrRegen %s --match-full-lines

// Loads of the same slot with different types must not be merged.

class O {}

function f(p: {x: O}, b: boolean): any {
  if (b) return (p: {x: O} | {x: number}).x;
  return (p: {x: O} | {x: void}).x;
}

(globalThis as any).f = f;

// Auto-generated content below. Please do not modify manually.

// CHECK:function f(p: exactObject(x: classInstance(O)), b: boolean): undefined|number|classInstance(O) [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:exactObject(x: classInstance(O))) %p: exactObject(x: classInstance(O))
// CHECK-NEXT:  %1 = LoadParamInst (:boolean) %b: boolean
// CHECK-NEXT:       CondBranchInst %1: boolean, %BB1, %BB2
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %3 = PrLoadInst (:number|classInstance(O)) %0: exactObject(x: classInstance(O)), 0: number, "x": string
// CHECK-NEXT:       ReturnInst %3: number|classInstance(O)
// CHECK-NEXT:%BB2:
// CHECK-NEXT:  %5 = PrLoadInst (:undefined|classInstance(O)) %0: exactObject(x: classInstance(O)), 0: number, "x": string
// CHECK-NEXT:       ReturnInst %5: undefined|classInstance(O)
// CHECK-NEXT:function_end

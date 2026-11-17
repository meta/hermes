/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=foo -Werror -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

function foo(x: number): C {
  // Ensure the target operand of new C() is populated at IRGen.
  return new C(x);
}

class C {
  x: number;
  constructor(x) {
    this.x = x;
  }
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, foo: any, C: any, ?C.prototype: classInstance()]

// CHECK:scope %VS1 [x: any]

// CHECK:function foo(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS1.x]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %0: environment, [%VS0.C]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:object) %4: any, type(object)
// CHECK-NEXT:  %6 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?C.prototype]: classInstance()
// CHECK-NEXT:  %7 = AllocTypedObjectInst (:object) %6: classInstance(), "x": string, 0: number
// CHECK-NEXT:  %8 = UnionNarrowTrustedInst (:classInstance(C)) %7: object
// CHECK-NEXT:  %9 = LoadFrameInst (:any) %1: environment, [%VS1.x]: any
// CHECK-NEXT:  %10 = CheckedTypeCastInst (:number) %9: any, type(number)
// CHECK-NEXT:  %11 = CallInst (:any) %5: object, %C(): functionCode, true: boolean, empty: any, %5: object, %8: classInstance(C), %10: number
// CHECK-NEXT:        ReturnInst %8: classInstance(C)
// CHECK-NEXT:function_end

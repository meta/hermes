/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Xdump-functions="" -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

// Verify that fn.call(thisArg, ...) on a typed function value lowers to a
// direct CallInst, with no LoadPropertyInst for "call" — matching the shape
// produced by $SHBuiltin.call.

function withThis(this: number, n: number): number {
  return this + n;
}
function noThis(a: number, b: string): boolean {
  return b.length > a;
}
withThis.call(10, 5);
noThis.call(undefined, 1, "hi");

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:scope %VS1 [exports: any, withThis: any, noThis: any]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:  %4 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %withThis(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: function(number), [%VS1.withThis]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:function(boolean)) %1: environment, %VS1: any, %noThis(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: function(boolean), [%VS1.noThis]: any
// CHECK-NEXT:  %8 = LoadFrameInst (:any) %1: environment, [%VS1.withThis]: any
// CHECK-NEXT:  %9 = CheckedTypeCastInst (:function(number)) %8: any, type(function(number))
// CHECK-NEXT:  %10 = CallInst (:any) %9: function(number), empty: any, false: boolean, empty: any, undefined: undefined, 10: number, 5: number
// CHECK-NEXT:  %11 = CheckedTypeCastInst (:number) %10: any, type(number)
// CHECK-NEXT:  %12 = LoadFrameInst (:any) %1: environment, [%VS1.noThis]: any
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:function(boolean)) %12: any, type(function(boolean))
// CHECK-NEXT:  %14 = CallInst (:any) %13: function(boolean), empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, 1: number, "hi": string
// CHECK-NEXT:  %15 = CheckedTypeCastInst (:boolean) %14: any, type(boolean)
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -typed -O0 -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

return function main() {
  let x: string = 'X';
  // Typecast needed right now because SH has 'any' as the variable type.
  return `hello${(x: string)}world`;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:       StoreStackInst undefined: undefined, %1: any
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) %0: environment, %VS0: any, %""(): functionCode
// CHECK-NEXT:  %4 = AllocTypedObjectInst (:object) empty: any
// CHECK-NEXT:  %5 = UnionNarrowTrustedInst (:exactObject()) %4: object
// CHECK-NEXT:  %6 = CallInst [njsf] (:any) %3: object, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %5: exactObject()
// CHECK-NEXT:       StoreStackInst %6: any, %1: any
// CHECK-NEXT:  %8 = LoadStackInst (:any) %1: any
// CHECK-NEXT:       ReturnInst %8: any
// CHECK-NEXT:function_end

// CHECK:scope %VS1 [exports: any, main: any]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:  %4 = CreateFunctionInst (:object) %1: environment, %VS1: any, %main(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: object, [%VS1.main]: any
// CHECK-NEXT:       ReturnInst %4: object
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [x: any]

// CHECK:function main(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.x]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, "X": string, [%VS2.x]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS2.x]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:string) %4: any, type(string)
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:string) %5: string, type(string)
// CHECK-NEXT:  %7 = StringConcatInst (:string) "hello": string, %6: string, "world": string
// CHECK-NEXT:       ReturnInst %7: string
// CHECK-NEXT:function_end

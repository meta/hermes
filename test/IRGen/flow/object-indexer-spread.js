/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -O0 -typed -dump-ir -Xdump-functions=main %s | %FileCheckOrRegen %s --match-full-lines

return function main() {
  let d: {[string]: number} = {a: 1};
  // Spreading the indexer object lowers to copyDataProperties; the named
  // property lowers to a dynamic property define.
  let r: {[string]: number | string} = {...d, x: "s"};
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, main: any]

// CHECK:scope %VS1 [d: any, r: any]

// CHECK:function main(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.d]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.r]: any
// CHECK-NEXT:  %4 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:       DefineOwnPropertyInst 1: number, %4: object, "a": string, true: boolean
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: object, [%VS1.d]: any
// CHECK-NEXT:  %7 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:  %8 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %9 = CheckedTypeCastInst (:object) %8: any, type(object)
// CHECK-NEXT:  %10 = CallBuiltinInst (:any) [HermesBuiltin.copyDataProperties]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %7: object, %9: object
// CHECK-NEXT:        DefineOwnPropertyInst "s": string, %7: object, "x": string, true: boolean
// CHECK-NEXT:        StoreFrameInst %1: environment, %7: object, [%VS1.r]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

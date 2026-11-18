/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O0 -fno-std-globals -typed -dump-ir -Xdump-functions=main %s | %FileCheckOrRegen %s --match-full-lines

'use strict';

return function main() {
  let x: [number, bool] = [1, true]
  let y: number = x[0]
  let z: bool = x[1]
  x[0] = 2;
  x[1] = false;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, main: any]

// CHECK:scope %VS1 [x: any, y: any, z: any]

// CHECK:function main(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.x]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.y]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.z]: any
// CHECK-NEXT:  %5 = AllocTypedObjectInst (:object) empty: any, "0": string, 1: number, "1": string, true: boolean
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:tuple(number, boolean)) %5: object
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: tuple(number, boolean), [%VS1.x]: any
// CHECK-NEXT:  %8 = LoadFrameInst (:any) %1: environment, [%VS1.x]: any
// CHECK-NEXT:  %9 = CheckedTypeCastInst (:tuple(number, boolean)) %8: any, type(tuple(number, boolean))
// CHECK-NEXT:  %10 = PrLoadInst (:number) %9: tuple(number, boolean), 0: number, "0": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: number, [%VS1.y]: any
// CHECK-NEXT:  %12 = LoadFrameInst (:any) %1: environment, [%VS1.x]: any
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:tuple(number, boolean)) %12: any, type(tuple(number, boolean))
// CHECK-NEXT:  %14 = PrLoadInst (:boolean) %13: tuple(number, boolean), 1: number, "1": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %14: boolean, [%VS1.z]: any
// CHECK-NEXT:  %16 = LoadFrameInst (:any) %1: environment, [%VS1.x]: any
// CHECK-NEXT:  %17 = CheckedTypeCastInst (:tuple(number, boolean)) %16: any, type(tuple(number, boolean))
// CHECK-NEXT:        PrStoreInst 2: number, %17: tuple(number, boolean), 0: number, "0": string, true: boolean
// CHECK-NEXT:  %19 = LoadFrameInst (:any) %1: environment, [%VS1.x]: any
// CHECK-NEXT:  %20 = CheckedTypeCastInst (:tuple(number, boolean)) %19: any, type(tuple(number, boolean))
// CHECK-NEXT:        PrStoreInst false: boolean, %20: tuple(number, boolean), 1: number, "1": string, true: boolean
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

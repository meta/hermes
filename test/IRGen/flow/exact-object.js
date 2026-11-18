/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -O0 -typed -dump-ir -Xdump-functions=main %s | %FileCheckOrRegen %s --match-full-lines

return function main() {
  let t: {x: number} = {x: 3};
  let tx: number = t.x;
  t.x = 5;

  let t2: {x: number, y: number} = {x: 3, x: 4, y: t.x};
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, main: any]

// CHECK:scope %VS1 [t: any, tx: any, t2: any]

// CHECK:function main(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.t]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.tx]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.t2]: any
// CHECK-NEXT:  %5 = AllocTypedObjectInst (:object) empty: any, "x": string, 3: number
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:exactObject(x: number)) %5: object
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: exactObject(x: number), [%VS1.t]: any
// CHECK-NEXT:  %8 = LoadFrameInst (:any) %1: environment, [%VS1.t]: any
// CHECK-NEXT:  %9 = CheckedTypeCastInst (:exactObject(x: number)) %8: any, type(exactObject(x: number))
// CHECK-NEXT:  %10 = PrLoadInst (:number) %9: exactObject(x: number), 0: number, "x": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: number, [%VS1.tx]: any
// CHECK-NEXT:  %12 = LoadFrameInst (:any) %1: environment, [%VS1.t]: any
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:exactObject(x: number)) %12: any, type(exactObject(x: number))
// CHECK-NEXT:        PrStoreInst 5: number, %13: exactObject(x: number), 0: number, "x": string, true: boolean
// CHECK-NEXT:  %15 = LoadFrameInst (:any) %1: environment, [%VS1.t]: any
// CHECK-NEXT:  %16 = CheckedTypeCastInst (:exactObject(x: number)) %15: any, type(exactObject(x: number))
// CHECK-NEXT:  %17 = PrLoadInst (:number) %16: exactObject(x: number), 0: number, "x": string
// CHECK-NEXT:  %18 = AllocTypedObjectInst (:object) empty: any, "x": string, 4: number, "y": string, %17: number
// CHECK-NEXT:  %19 = UnionNarrowTrustedInst (:exactObject(x: number, y: number)) %18: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %19: exactObject(x: number, y: number), [%VS1.t2]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

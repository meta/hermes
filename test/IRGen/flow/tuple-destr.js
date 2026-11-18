/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O0 -fno-std-globals -typed -dump-ir -Xdump-functions=main %s | %FileCheckOrRegen %s --match-full-lines

'use strict';

return function main() {
  let inner: [number, string] = [2, 'asdf'];
  let outer: [number, bool, [number, string]] = [1, true, inner];
  let [x, y, [a, b]] = outer;
  let i: number;
  let j: string;
  [i, j] = inner;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, main: any]

// CHECK:scope %VS1 [inner: any, outer: any, x: any, y: any, a: any, b: any, i: any, j: any]

// CHECK:function main(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.inner]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.outer]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.x]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.y]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.a]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.b]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.i]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.j]: any
// CHECK-NEXT:  %10 = AllocTypedObjectInst (:object) empty: any, "0": string, 2: number, "1": string, "asdf": string
// CHECK-NEXT:  %11 = UnionNarrowTrustedInst (:tuple(number, string)) %10: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %11: tuple(number, string), [%VS1.inner]: any
// CHECK-NEXT:  %13 = LoadFrameInst (:any) %1: environment, [%VS1.inner]: any
// CHECK-NEXT:  %14 = CheckedTypeCastInst (:tuple(number, string)) %13: any, type(tuple(number, string))
// CHECK-NEXT:  %15 = AllocTypedObjectInst (:object) empty: any, "0": string, 1: number, "1": string, true: boolean, "2": string, %14: tuple(number, string)
// CHECK-NEXT:  %16 = UnionNarrowTrustedInst (:tuple(number, boolean, tuple(number, string))) %15: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %16: tuple(number, boolean, tuple(number, string)), [%VS1.outer]: any
// CHECK-NEXT:  %18 = LoadFrameInst (:any) %1: environment, [%VS1.outer]: any
// CHECK-NEXT:  %19 = CheckedTypeCastInst (:tuple(number, boolean, tuple(number, string))) %18: any, type(tuple(number, boolean, tuple(number, string)))
// CHECK-NEXT:  %20 = PrLoadInst (:number) %19: tuple(number, boolean, tuple(number, string)), 0: number, "0": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %20: number, [%VS1.x]: any
// CHECK-NEXT:  %22 = PrLoadInst (:boolean) %19: tuple(number, boolean, tuple(number, string)), 1: number, "1": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %22: boolean, [%VS1.y]: any
// CHECK-NEXT:  %24 = PrLoadInst (:tuple(number, string)) %19: tuple(number, boolean, tuple(number, string)), 2: number, "2": string
// CHECK-NEXT:  %25 = PrLoadInst (:number) %24: tuple(number, string), 0: number, "0": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %25: number, [%VS1.a]: any
// CHECK-NEXT:  %27 = PrLoadInst (:string) %24: tuple(number, string), 1: number, "1": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %27: string, [%VS1.b]: any
// CHECK-NEXT:        StoreFrameInst %1: environment, undefined: undefined, [%VS1.i]: any
// CHECK-NEXT:        StoreFrameInst %1: environment, undefined: undefined, [%VS1.j]: any
// CHECK-NEXT:  %31 = LoadFrameInst (:any) %1: environment, [%VS1.inner]: any
// CHECK-NEXT:  %32 = CheckedTypeCastInst (:tuple(number, string)) %31: any, type(tuple(number, string))
// CHECK-NEXT:  %33 = PrLoadInst (:number) %32: tuple(number, string), 0: number, "0": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %33: number, [%VS1.i]: any
// CHECK-NEXT:  %35 = PrLoadInst (:string) %32: tuple(number, string), 1: number, "1": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %35: string, [%VS1.j]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -O0 -typed -dump-ir -Xdump-functions=main %s | %FileCheckOrRegen %s --match-full-lines

return function main() {
  let d: {[string]: number} = {a: 1};
  // Read lowers to a dynamic load plus a checked cast to number.
  let n: number = d["x"];
  // Write lowers to a dynamic store.
  d["y"] = 3;
  // A function read from an indexer can be called: the callee is narrowed
  // from `(() => number) | void` to `() => number` before the call.
  let f: {[string]: () => number} = {};
  let r: number = f["g"]();
  // Dot access lowers identically to computed access on an indexer object.
  let dn: number = d.x;
  d.y = 4;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, main: any]

// CHECK:scope %VS1 [d: any, n: any, f: any, r: any, dn: any]

// CHECK:function main(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.d]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.n]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.f]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.r]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.dn]: any
// CHECK-NEXT:  %7 = AllocObjectLiteralInst (:object) empty: any, "a": string, 1: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %7: object, [%VS1.d]: any
// CHECK-NEXT:  %9 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %10 = CheckedTypeCastInst (:object) %9: any, type(object)
// CHECK-NEXT:  %11 = LoadPropertyInst (:undefined|number) %10: object, "x": string
// CHECK-NEXT:  %12 = CheckedTypeCastInst (:number) %11: undefined|number, type(number)
// CHECK-NEXT:        StoreFrameInst %1: environment, %12: number, [%VS1.n]: any
// CHECK-NEXT:  %14 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %15 = CheckedTypeCastInst (:object) %14: any, type(object)
// CHECK-NEXT:        StorePropertyStrictInst 3: number, %15: object, "y": string
// CHECK-NEXT:  %17 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:        StoreFrameInst %1: environment, %17: object, [%VS1.f]: any
// CHECK-NEXT:  %19 = LoadFrameInst (:any) %1: environment, [%VS1.f]: any
// CHECK-NEXT:  %20 = CheckedTypeCastInst (:object) %19: any, type(object)
// CHECK-NEXT:  %21 = LoadPropertyInst (:undefined|function(number)) %20: object, "g": string
// CHECK-NEXT:  %22 = CheckedTypeCastInst (:function(number)) %21: undefined|function(number), type(function(number))
// CHECK-NEXT:  %23 = CallInst [njsf] (:number) %22: function(number), empty: any, false: boolean, empty: any, undefined: undefined, %20: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %23: number, [%VS1.r]: any
// CHECK-NEXT:  %25 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %26 = CheckedTypeCastInst (:object) %25: any, type(object)
// CHECK-NEXT:  %27 = LoadPropertyInst (:undefined|number) %26: object, "x": string
// CHECK-NEXT:  %28 = CheckedTypeCastInst (:number) %27: undefined|number, type(number)
// CHECK-NEXT:        StoreFrameInst %1: environment, %28: number, [%VS1.dn]: any
// CHECK-NEXT:  %30 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %31 = CheckedTypeCastInst (:object) %30: any, type(object)
// CHECK-NEXT:        StorePropertyStrictInst 4: number, %31: object, "y": string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

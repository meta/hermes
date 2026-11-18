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
// CHECK-NEXT:  %7 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:       DefineOwnPropertyInst 1: number, %7: object, "a": string, true: boolean
// CHECK-NEXT:       StoreFrameInst %1: environment, %7: object, [%VS1.d]: any
// CHECK-NEXT:  %10 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %11 = CheckedTypeCastInst (:object) %10: any, type(object)
// CHECK-NEXT:  %12 = LoadPropertyInst (:undefined|number) %11: object, "x": string
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:number) %12: undefined|number, type(number)
// CHECK-NEXT:        StoreFrameInst %1: environment, %13: number, [%VS1.n]: any
// CHECK-NEXT:  %15 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %16 = CheckedTypeCastInst (:object) %15: any, type(object)
// CHECK-NEXT:        StorePropertyStrictInst 3: number, %16: object, "y": string
// CHECK-NEXT:  %18 = AllocTypedObjectInst (:object) empty: any
// CHECK-NEXT:  %19 = UnionNarrowTrustedInst (:exactObject()) %18: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %19: exactObject(), [%VS1.f]: any
// CHECK-NEXT:  %21 = LoadFrameInst (:any) %1: environment, [%VS1.f]: any
// CHECK-NEXT:  %22 = CheckedTypeCastInst (:object) %21: any, type(object)
// CHECK-NEXT:  %23 = LoadPropertyInst (:undefined|function(number)) %22: object, "g": string
// CHECK-NEXT:  %24 = CheckedTypeCastInst (:function(number)) %23: undefined|function(number), type(function(number))
// CHECK-NEXT:  %25 = CallInst [njsf] (:number) %24: function(number), empty: any, false: boolean, empty: any, undefined: undefined, %22: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %25: number, [%VS1.r]: any
// CHECK-NEXT:  %27 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %28 = CheckedTypeCastInst (:object) %27: any, type(object)
// CHECK-NEXT:  %29 = LoadPropertyInst (:undefined|number) %28: object, "x": string
// CHECK-NEXT:  %30 = CheckedTypeCastInst (:number) %29: undefined|number, type(number)
// CHECK-NEXT:        StoreFrameInst %1: environment, %30: number, [%VS1.dn]: any
// CHECK-NEXT:  %32 = LoadFrameInst (:any) %1: environment, [%VS1.d]: any
// CHECK-NEXT:  %33 = CheckedTypeCastInst (:object) %32: any, type(object)
// CHECK-NEXT:        StorePropertyStrictInst 4: number, %33: object, "y": string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

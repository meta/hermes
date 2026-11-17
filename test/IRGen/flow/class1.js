/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -typed -dump-ir %s | %FileCheckOrRegen %s --match-full-lines

(function(){
class Vec2D {
    x: number;
    y: number;

    constructor(x: number, y: number) {
        this.x = Math.sqrt(10);
        this.y = y;
    }
}

function dotProduct(a: Vec2D, b: Vec2D) {
    return a.x*b.x + a.y*b.y;
}

return [dotProduct, Vec2D];
})();

// Auto-generated content below. Please do not modify manually.

// CHECK:function global(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateFunctionInst (:object) empty: any, empty: any, %Array(): functionCode
// CHECK-NEXT:  %1 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %2 = UnionNarrowTrustedInst (:classInstance()) %1: object
// CHECK-NEXT:       StorePropertyStrictInst %2: classInstance(), %0: object, "prototype": string
// CHECK-NEXT:  %4 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 1#"(): functionCode
// CHECK-NEXT:  %5 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:classInstance()) %5: object
// CHECK-NEXT:       StorePropertyStrictInst %6: classInstance(), %4: object, "prototype": string
// CHECK-NEXT:  %8 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 2#"(): functionCode
// CHECK-NEXT:  %9 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %8: object, "prototype": string
// CHECK-NEXT:  %12 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 3#"(): functionCode
// CHECK-NEXT:  %13 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %14 = UnionNarrowTrustedInst (:classInstance()) %13: object
// CHECK-NEXT:        StorePropertyStrictInst %14: classInstance(), %12: object, "prototype": string
// CHECK-NEXT:  %16 = CreateFunctionInst (:function(undefined)) empty: any, empty: any, %Symbol(): functionCode
// CHECK-NEXT:  %17 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance()) %17: object
// CHECK-NEXT:        StorePropertyStrictInst %18: classInstance(), %16: function(undefined), "prototype": string
// CHECK-NEXT:  %20 = CreateFunctionInst (:object) empty: any, empty: any, %Object(): functionCode
// CHECK-NEXT:  %21 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %22 = UnionNarrowTrustedInst (:classInstance()) %21: object
// CHECK-NEXT:        StorePropertyStrictInst %22: classInstance(), %20: object, "prototype": string
// CHECK-NEXT:  %24 = CreateFunctionInst (:function(any)) empty: any, empty: any, %dotProduct(): functionCode
// CHECK-NEXT:  %25 = CreateFunctionInst (:function(undefined)) empty: any, empty: any, %Vec2D(): functionCode
// CHECK-NEXT:  %26 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %27 = UnionNarrowTrustedInst (:classInstance()) %26: object
// CHECK-NEXT:        StorePropertyStrictInst %27: classInstance(), %25: function(undefined), "prototype": string
// CHECK-NEXT:  %29 = AllocFastArrayInst (:object) 2: number, %14: classInstance()
// CHECK-NEXT:  %30 = UnionNarrowTrustedInst (:array(function(any)|function(undefined))) %29: object
// CHECK-NEXT:        FastArrayPushInst %24: function(any), %30: array(function(any)|function(undefined))
// CHECK-NEXT:        FastArrayPushInst %25: function(undefined), %30: array(function(any)|function(undefined))
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function Array(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 1#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 2#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 3#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor Symbol(): any [typed,noReturn]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = TryLoadGlobalPropertyInst (:any) globalObject: object, "TypeError": string
// CHECK-NEXT:  %1 = CreateThisInst (:undefined|object) %0: any, %0: any, empty: any
// CHECK-NEXT:  %2 = CallInst (:any) %0: any, empty: any, false: boolean, empty: any, %0: any, %1: undefined|object, "Symbol is not a constructor": string
// CHECK-NEXT:  %3 = GetConstructedObjectInst (:object) %1: undefined|object, %2: any
// CHECK-NEXT:       ThrowInst %3: object
// CHECK-NEXT:function_end

// CHECK:function Object(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function dotProduct(a: classInstance(Vec2D), b: classInstance(Vec2D)): number [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Vec2D)) %a: classInstance(Vec2D)
// CHECK-NEXT:  %1 = LoadParamInst (:classInstance(Vec2D)) %b: classInstance(Vec2D)
// CHECK-NEXT:  %2 = PrLoadInst (:number) %0: classInstance(Vec2D), 0: number, "x": string
// CHECK-NEXT:  %3 = PrLoadInst (:number) %1: classInstance(Vec2D), 0: number, "x": string
// CHECK-NEXT:  %4 = FMultiplyInst (:number) %2: number, %3: number
// CHECK-NEXT:  %5 = PrLoadInst (:number) %0: classInstance(Vec2D), 1: number, "y": string
// CHECK-NEXT:  %6 = PrLoadInst (:number) %1: classInstance(Vec2D), 1: number, "y": string
// CHECK-NEXT:  %7 = FMultiplyInst (:number) %5: number, %6: number
// CHECK-NEXT:  %8 = FAddInst (:number) %4: number, %7: number
// CHECK-NEXT:       ReturnInst %8: number
// CHECK-NEXT:function_end

// CHECK:base constructor Vec2D(x: number, y: number): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Vec2D)) %<this>: classInstance(Vec2D)
// CHECK-NEXT:  %1 = LoadParamInst (:number) %y: number
// CHECK-NEXT:  %2 = CallBuiltinInst (:number) [Math.sqrt]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, 10: number
// CHECK-NEXT:       PrStoreInst %2: number, %0: classInstance(Vec2D), 0: number, "x": string, true: boolean
// CHECK-NEXT:       PrStoreInst %1: number, %0: classInstance(Vec2D), 1: number, "y": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

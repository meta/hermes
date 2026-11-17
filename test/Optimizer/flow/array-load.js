/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -dump-ir -O %s | %FileCheckOrRegen %s --match-full-lines

function main() {
  let x: number[] = [1, 2, 3];
  return x[0] + 1;
}

// Prevent the function from being optimized out.
sink(main);

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [?Array.prototype: classInstance()]

// CHECK:function global(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = CreateFunctionInst (:object) empty: any, empty: any, %Array(): functionCode
// CHECK-NEXT:  %2 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %3 = UnionNarrowTrustedInst (:classInstance()) %2: object
// CHECK-NEXT:       StorePropertyStrictInst %3: classInstance(), %1: object, "prototype": string
// CHECK-NEXT:  %5 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 1#"(): functionCode
// CHECK-NEXT:  %6 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %7 = UnionNarrowTrustedInst (:classInstance()) %6: object
// CHECK-NEXT:       StorePropertyStrictInst %7: classInstance(), %5: object, "prototype": string
// CHECK-NEXT:  %9 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 2#"(): functionCode
// CHECK-NEXT:  %10 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %11 = UnionNarrowTrustedInst (:classInstance()) %10: object
// CHECK-NEXT:        StorePropertyStrictInst %11: classInstance(), %9: object, "prototype": string
// CHECK-NEXT:  %13 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 3#"(): functionCode
// CHECK-NEXT:  %14 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %15 = UnionNarrowTrustedInst (:classInstance()) %14: object
// CHECK-NEXT:        StoreFrameInst %0: environment, %15: classInstance(), [%VS0.?Array.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %15: classInstance(), %13: object, "prototype": string
// CHECK-NEXT:  %18 = CreateFunctionInst (:object) empty: any, empty: any, %Symbol(): functionCode
// CHECK-NEXT:  %19 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %20 = UnionNarrowTrustedInst (:classInstance()) %19: object
// CHECK-NEXT:        StorePropertyStrictInst %20: classInstance(), %18: object, "prototype": string
// CHECK-NEXT:  %22 = CreateFunctionInst (:object) empty: any, empty: any, %Object(): functionCode
// CHECK-NEXT:  %23 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %24 = UnionNarrowTrustedInst (:classInstance()) %23: object
// CHECK-NEXT:        StorePropertyStrictInst %24: classInstance(), %22: object, "prototype": string
// CHECK-NEXT:  %26 = CreateFunctionInst (:object) %0: environment, %VS0: any, %main(): functionCode
// CHECK-NEXT:  %27 = TryLoadGlobalPropertyInst (:any) globalObject: object, "sink": string
// CHECK-NEXT:  %28 = CallInst (:any) %27: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %26: object
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

// CHECK:function main(): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype]: classInstance()
// CHECK-NEXT:  %2 = AllocFastArrayInst (:object) 3: number, %1: classInstance()
// CHECK-NEXT:       FastArrayPushInst 1: number, %2: object
// CHECK-NEXT:       FastArrayPushInst 2: number, %2: object
// CHECK-NEXT:       FastArrayPushInst 3: number, %2: object
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:classInstance(Array)) %2: object, type(classInstance(Array))
// CHECK-NEXT:  %7 = FastArrayLoadInst (:number) %6: classInstance(Array), 0: number
// CHECK-NEXT:  %8 = FAddInst (:number) %7: number, 1: number
// CHECK-NEXT:       ReturnInst %8: number
// CHECK-NEXT:function_end

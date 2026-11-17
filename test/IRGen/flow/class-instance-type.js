/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class Animal {
  legs: number;
  constructor() {
    this.legs = 4;
  }
}

class Dog extends Animal {
  name: string;
  constructor() {
    super();
    this.name = 'rex';
  }
}

function make(): Dog {
  return new Dog();
}

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
// CHECK-NEXT:  %12 = CreateFunctionInst (:object) empty: any, empty: any, %Symbol(): functionCode
// CHECK-NEXT:  %13 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %14 = UnionNarrowTrustedInst (:classInstance()) %13: object
// CHECK-NEXT:        StorePropertyStrictInst %14: classInstance(), %12: object, "prototype": string
// CHECK-NEXT:  %16 = CreateFunctionInst (:object) empty: any, empty: any, %Object(): functionCode
// CHECK-NEXT:  %17 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance()) %17: object
// CHECK-NEXT:        StorePropertyStrictInst %18: classInstance(), %16: object, "prototype": string
// CHECK-NEXT:  %20 = CreateFunctionInst (:object) empty: any, empty: any, %Animal(): functionCode
// CHECK-NEXT:  %21 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %22 = UnionNarrowTrustedInst (:classInstance()) %21: object
// CHECK-NEXT:        StorePropertyStrictInst %22: classInstance(), %20: object, "prototype": string
// CHECK-NEXT:  %24 = CreateFunctionInst (:object) empty: any, empty: any, %Dog(): functionCode
// CHECK-NEXT:  %25 = AllocTypedNonEnumObjectInst (:object) %22: classInstance()
// CHECK-NEXT:  %26 = UnionNarrowTrustedInst (:classInstance()) %25: object
// CHECK-NEXT:        StorePropertyStrictInst %26: classInstance(), %24: object, "prototype": string
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

// CHECK:base constructor Animal(): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Animal)) %<this>: classInstance(Animal)
// CHECK-NEXT:       PrStoreInst 4: number, %0: classInstance(Animal), 0: number, "legs": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:derived constructor Dog(): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Dog)) %<this>: classInstance(Dog)
// CHECK-NEXT:       PrStoreInst 4: number, %0: classInstance(Dog), 0: number, "legs": string, true: boolean
// CHECK-NEXT:       PrStoreInst "rex": string, %0: classInstance(Dog), 1: number, "name": string, false: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

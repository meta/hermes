/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -O0 -typed -dump-ir -Xno-dump-functions=global %s | %FileCheckOrRegen %s --match-full-lines

class ID<T> {
  val: T;

  constructor(val: T) {
    this.val = val;
  }
}

const i1: ID<number> = new ID<number>(1);
const n: number = i1.val;

const i2: ID<string> = new ID<string>('abc');
const s: string = i2.val;

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:scope %VS1 [exports: any, i1: any, n: any, i2: any, s: any, ID: any, ID#1: any, ?ID.prototype: classInstance(), ?ID.prototype#1: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.i1]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.n]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.i2]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.s]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.ID]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.ID#1]: any
// CHECK-NEXT:  %10 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %ID(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: function(undefined), [%VS1.ID]: any
// CHECK-NEXT:  %12 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %13 = UnionNarrowTrustedInst (:classInstance()) %12: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %13: classInstance(), [%VS1.?ID.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %13: classInstance(), %10: function(undefined), "prototype": string
// CHECK-NEXT:  %16 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %"ID 1#"(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %16: function(undefined), [%VS1.ID#1]: any
// CHECK-NEXT:  %18 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %19 = UnionNarrowTrustedInst (:classInstance()) %18: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %19: classInstance(), [%VS1.?ID.prototype#1]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %19: classInstance(), %16: function(undefined), "prototype": string
// CHECK-NEXT:  %22 = LoadFrameInst (:any) %1: environment, [%VS1.ID]: any
// CHECK-NEXT:  %23 = CheckedTypeCastInst (:function(undefined)) %22: any, type(function(undefined))
// CHECK-NEXT:  %24 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?ID.prototype]: classInstance()
// CHECK-NEXT:  %25 = AllocTypedObjectInst (:object) %24: classInstance(), "val": string, 0: number
// CHECK-NEXT:  %26 = UnionNarrowTrustedInst (:classInstance(ID)) %25: object
// CHECK-NEXT:  %27 = CallInst (:any) %23: function(undefined), %ID(): functionCode, true: boolean, empty: any, %23: function(undefined), %26: classInstance(ID), 1: number
// CHECK-NEXT:        StoreFrameInst %1: environment, %26: classInstance(ID), [%VS1.i1]: any
// CHECK-NEXT:  %29 = LoadFrameInst (:any) %1: environment, [%VS1.i1]: any
// CHECK-NEXT:  %30 = CheckedTypeCastInst (:classInstance(ID)) %29: any, type(classInstance(ID))
// CHECK-NEXT:  %31 = PrLoadInst (:number) %30: classInstance(ID), 0: number, "val": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %31: number, [%VS1.n]: any
// CHECK-NEXT:  %33 = LoadFrameInst (:any) %1: environment, [%VS1.ID#1]: any
// CHECK-NEXT:  %34 = CheckedTypeCastInst (:function(undefined)) %33: any, type(function(undefined))
// CHECK-NEXT:  %35 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?ID.prototype#1]: classInstance()
// CHECK-NEXT:  %36 = AllocTypedObjectInst (:object) %35: classInstance(), "val": string, "": string
// CHECK-NEXT:  %37 = UnionNarrowTrustedInst (:classInstance(ID)) %36: object
// CHECK-NEXT:  %38 = CallInst (:any) %34: function(undefined), %"ID 1#"(): functionCode, true: boolean, empty: any, %34: function(undefined), %37: classInstance(ID), "abc": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %37: classInstance(ID), [%VS1.i2]: any
// CHECK-NEXT:  %40 = LoadFrameInst (:any) %1: environment, [%VS1.i2]: any
// CHECK-NEXT:  %41 = CheckedTypeCastInst (:classInstance(ID)) %40: any, type(classInstance(ID))
// CHECK-NEXT:  %42 = PrLoadInst (:string) %41: classInstance(ID), 0: number, "val": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %42: string, [%VS1.s]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [val: any]

// CHECK:base constructor ID(val: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(ID)) %<this>: classInstance(ID)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS2: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:number) %val: number
// CHECK-NEXT:       StoreFrameInst %2: environment, %3: number, [%VS2.val]: any
// CHECK-NEXT:  %5 = LoadFrameInst (:any) %2: environment, [%VS2.val]: any
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:number) %5: any, type(number)
// CHECK-NEXT:       PrStoreInst %6: number, %0: classInstance(ID), 0: number, "val": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [val: any]

// CHECK:base constructor "ID 1#"(val: string): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(ID)) %<this>: classInstance(ID)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS3: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:string) %val: string
// CHECK-NEXT:       StoreFrameInst %2: environment, %3: string, [%VS3.val]: any
// CHECK-NEXT:  %5 = LoadFrameInst (:any) %2: environment, [%VS3.val]: any
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:string) %5: any, type(string)
// CHECK-NEXT:       PrStoreInst %6: string, %0: classInstance(ID), 0: number, "val": string, false: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

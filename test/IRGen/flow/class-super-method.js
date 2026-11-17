/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class A {
  x: number;

  constructor(x: number) {
    this.x = x;
  }

  f(): number {
    return this.x * 10;
  }
}

class B extends A {
  constructor(x: number) {
    super(x);
  }

  f(): number {
    return super.f() + 23;
  }
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:       StoreStackInst undefined: undefined, %1: any
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) %0: environment, %VS0: any, %""(): functionCode
// CHECK-NEXT:  %4 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:  %5 = CallInst [njsf] (:any) %3: object, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %4: object
// CHECK-NEXT:       StoreStackInst %5: any, %1: any
// CHECK-NEXT:  %7 = LoadStackInst (:any) %1: any
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:function_end

// CHECK:scope %VS1 [exports: any, A: any, B: any, ?A.prototype: classInstance(), ?B.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.A]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.B]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %A(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: function(undefined), [%VS1.A]: any
// CHECK-NEXT:  %8 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %f(): functionCode
// CHECK-NEXT:  %9 = AllocTypedNonEnumObjectInst (:object) null: null, "f": string, %8: function(number)
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: classInstance(), [%VS1.?A.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %6: function(undefined), "prototype": string
// CHECK-NEXT:  %13 = LoadFrameInst (:any) %1: environment, [%VS1.A]: any
// CHECK-NEXT:  %14 = CheckedTypeCastInst (:function(undefined)) %13: any, type(function(undefined))
// CHECK-NEXT:  %15 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %B(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %15: function(undefined), [%VS1.B]: any
// CHECK-NEXT:  %17 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?A.prototype]: classInstance()
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:object) %17: classInstance()
// CHECK-NEXT:  %19 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %"f 1#"(): functionCode
// CHECK-NEXT:  %20 = AllocTypedNonEnumObjectInst (:object) %18: object, "f": string, %19: function(number)
// CHECK-NEXT:  %21 = UnionNarrowTrustedInst (:classInstance()) %20: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %21: classInstance(), [%VS1.?B.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %21: classInstance(), %15: function(undefined), "prototype": string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [x: any]

// CHECK:base constructor A(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(A)) %<this>: classInstance(A)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS2: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %2: environment, %3: number, [%VS2.x]: any
// CHECK-NEXT:  %5 = LoadFrameInst (:any) %2: environment, [%VS2.x]: any
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:number) %5: any, type(number)
// CHECK-NEXT:       PrStoreInst %6: number, %0: classInstance(A), 0: number, "x": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function f(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(A)) %<this>: classInstance(A)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS3: any, %1: environment
// CHECK-NEXT:  %3 = PrLoadInst (:number) %0: classInstance(A), 0: number, "x": string
// CHECK-NEXT:  %4 = BinaryMultiplyInst (:any) %3: number, 10: number
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:number) %4: any, type(number)
// CHECK-NEXT:       ReturnInst %5: number
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [x: any]

// CHECK:derived constructor B(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(B)) %<this>: classInstance(B)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS4: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %2: environment, %3: number, [%VS4.x]: any
// CHECK-NEXT:  %5 = LoadFrameInst (:any) %1: environment, [%VS1.A]: any
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:function(undefined)) %5: any, type(function(undefined))
// CHECK-NEXT:  %7 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %8 = LoadFrameInst (:any) %2: environment, [%VS4.x]: any
// CHECK-NEXT:  %9 = CheckedTypeCastInst (:number) %8: any, type(number)
// CHECK-NEXT:  %10 = CallInst [njsf] (:undefined) %6: function(undefined), empty: any, false: boolean, empty: any, %7: object, %0: classInstance(B), %9: number
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 []

// CHECK:function "f 1#"(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(B)) %<this>: classInstance(B)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS5: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?A.prototype]: classInstance()
// CHECK-NEXT:  %4 = PrLoadInst (:function(number)) %3: classInstance(), 0: number, "f": string
// CHECK-NEXT:  %5 = CallInst [njsf] (:number) %4: function(number), empty: any, false: boolean, empty: any, undefined: undefined, %0: classInstance(B)
// CHECK-NEXT:  %6 = BinaryAddInst (:any) %5: number, 23: number
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:number) %6: any, type(number)
// CHECK-NEXT:       ReturnInst %7: number
// CHECK-NEXT:function_end

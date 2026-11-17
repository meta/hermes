/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -O0 -fno-inline -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class C {
  inherited(): number {
    return 1;
  }
}

class D extends C {
  constructor() {
    super();
  }
}

new D().inherited();

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

// CHECK:scope %VS1 [exports: any, C: any, D: any, ?C.prototype: classInstance(), ?D.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.D]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: object, [%VS1.C]: any
// CHECK-NEXT:  %8 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %inherited(): functionCode
// CHECK-NEXT:  %9 = AllocTypedNonEnumObjectInst (:object) null: null, "inherited": string, %8: function(number)
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %6: object, "prototype": string
// CHECK-NEXT:  %13 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %14 = CheckedTypeCastInst (:object) %13: any, type(object)
// CHECK-NEXT:  %15 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %D(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %15: function(undefined), [%VS1.D]: any
// CHECK-NEXT:  %17 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:object) %17: classInstance()
// CHECK-NEXT:  %19 = PrLoadInst (:function(number)) %18: object, 0: number, "inherited": string
// CHECK-NEXT:  %20 = AllocTypedNonEnumObjectInst (:object) %18: object, "inherited": string, %19: function(number)
// CHECK-NEXT:  %21 = UnionNarrowTrustedInst (:classInstance()) %20: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %21: classInstance(), [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %21: classInstance(), %15: function(undefined), "prototype": string
// CHECK-NEXT:  %24 = LoadFrameInst (:any) %1: environment, [%VS1.D]: any
// CHECK-NEXT:  %25 = CheckedTypeCastInst (:function(undefined)) %24: any, type(function(undefined))
// CHECK-NEXT:  %26 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:  %27 = AllocTypedObjectInst (:object) %26: classInstance()
// CHECK-NEXT:  %28 = UnionNarrowTrustedInst (:classInstance(D)) %27: object
// CHECK-NEXT:  %29 = CallInst (:any) %25: function(undefined), %D(): functionCode, true: boolean, empty: any, %25: function(undefined), %28: classInstance(D)
// CHECK-NEXT:  %30 = TypedLoadParentInst (:object) %28: classInstance(D)
// CHECK-NEXT:  %31 = PrLoadInst (:function(number)) %30: object, 0: number, "inherited": string
// CHECK-NEXT:  %32 = CallInst [njsf] (:number) %31: function(number), %inherited(): functionCode, true: boolean, empty: any, undefined: undefined, %28: classInstance(D)
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function C(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function inherited(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

// CHECK:scope %VS4 []

// CHECK:derived constructor D(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(D)) %<this>: classInstance(D)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS4: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %4 = CheckedTypeCastInst (:object) %3: any, type(object)
// CHECK-NEXT:  %5 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %6 = CallInst (:any) %4: object, empty: any, false: boolean, empty: any, %5: object, %0: classInstance(D)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

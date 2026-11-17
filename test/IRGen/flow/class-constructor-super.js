/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class C {
  constructor() {
  }
}

class D extends C {
  constructor() {
    super();
  }
}

new D();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:       StoreStackInst undefined: undefined, %1: any
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) %0: environment, %VS0: any, %""(): functionCode
// CHECK-NEXT:  %4 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:  %5 = UnionNarrowTrustedInst (:exactObject()) %4: object
// CHECK-NEXT:  %6 = CallInst [njsf] (:any) %3: object, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %5: exactObject()
// CHECK-NEXT:       StoreStackInst %6: any, %1: any
// CHECK-NEXT:  %8 = LoadStackInst (:any) %1: any
// CHECK-NEXT:       ReturnInst %8: any
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
// CHECK-NEXT:  %6 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: function(undefined), [%VS1.C]: any
// CHECK-NEXT:  %8 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %9 = UnionNarrowTrustedInst (:classInstance()) %8: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %9: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %9: classInstance(), %6: function(undefined), "prototype": string
// CHECK-NEXT:  %12 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:function(undefined)) %12: any, type(function(undefined))
// CHECK-NEXT:  %14 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %D(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %14: function(undefined), [%VS1.D]: any
// CHECK-NEXT:  %16 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %17 = UnionNarrowTrustedInst (:object) %16: classInstance()
// CHECK-NEXT:  %18 = AllocTypedNonEnumObjectInst (:object) %17: object
// CHECK-NEXT:  %19 = UnionNarrowTrustedInst (:classInstance()) %18: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %19: classInstance(), [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %19: classInstance(), %14: function(undefined), "prototype": string
// CHECK-NEXT:  %22 = LoadFrameInst (:any) %1: environment, [%VS1.D]: any
// CHECK-NEXT:  %23 = CheckedTypeCastInst (:function(undefined)) %22: any, type(function(undefined))
// CHECK-NEXT:  %24 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:  %25 = AllocTypedObjectInst (:object) %24: classInstance()
// CHECK-NEXT:  %26 = UnionNarrowTrustedInst (:classInstance(D)) %25: object
// CHECK-NEXT:  %27 = CallInst (:any) %23: function(undefined), %D(): functionCode, true: boolean, empty: any, %23: function(undefined), %26: classInstance(D)
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:base constructor C(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:derived constructor D(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(D)) %<this>: classInstance(D)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS3: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %4 = CheckedTypeCastInst (:function(undefined)) %3: any, type(function(undefined))
// CHECK-NEXT:  %5 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %6 = CallInst [njsf] (:undefined) %4: function(undefined), empty: any, false: boolean, empty: any, %5: object, %0: classInstance(D)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

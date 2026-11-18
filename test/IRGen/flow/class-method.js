/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -O0 -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class C {
  method(): number {
    return 1;
  }
}

return new C().method();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:       StoreStackInst undefined: undefined, %1: any
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) %0: environment, %VS0: any, %""(): functionCode
// CHECK-NEXT:  %4 = AllocTypedObjectInst (:object) empty: any
// CHECK-NEXT:  %5 = UnionNarrowTrustedInst (:exactObject()) %4: object
// CHECK-NEXT:  %6 = CallInst [njsf] (:any) %3: object, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %5: exactObject()
// CHECK-NEXT:       StoreStackInst %6: any, %1: any
// CHECK-NEXT:  %8 = LoadStackInst (:any) %1: any
// CHECK-NEXT:       ReturnInst %8: any
// CHECK-NEXT:function_end

// CHECK:scope %VS1 [exports: any, C: any, ?C.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:  %5 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %5: object, [%VS1.C]: any
// CHECK-NEXT:  %7 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %method(): functionCode
// CHECK-NEXT:  %8 = AllocTypedNonEnumObjectInst (:object) null: null, "method": string, %7: function(number)
// CHECK-NEXT:  %9 = UnionNarrowTrustedInst (:classInstance()) %8: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %9: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %9: classInstance(), %5: object, "prototype": string
// CHECK-NEXT:  %12 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:object) %12: any, type(object)
// CHECK-NEXT:  %14 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %15 = AllocTypedObjectInst (:object) %14: classInstance()
// CHECK-NEXT:  %16 = UnionNarrowTrustedInst (:classInstance(C)) %15: object
// CHECK-NEXT:  %17 = TypedLoadParentInst (:object) %16: classInstance(C)
// CHECK-NEXT:  %18 = PrLoadInst (:function(number)) %17: object, 0: number, "method": string
// CHECK-NEXT:  %19 = CallInst [njsf] (:number) %18: function(number), %method(): functionCode, true: boolean, empty: any, undefined: undefined, %16: classInstance(C)
// CHECK-NEXT:        ReturnInst %19: number
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function C(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function method(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

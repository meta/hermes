/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

// Ensure there's only one copy of the C constructor.
try {} finally { class C {} }

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

// CHECK:scope %VS1 [exports: any, C: any, ?C.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       TryStartInst %BB1, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %5 = CatchInst (:any)
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:  %7 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %7: object, [%VS1.C]: any
// CHECK-NEXT:  %9 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %7: object, "prototype": string
// CHECK-NEXT:        ThrowInst %5: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:%BB3:
// CHECK-NEXT:        TryEndInst %BB1, %BB4
// CHECK-NEXT:%BB4:
// CHECK-NEXT:        StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:  %17 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %17: object, [%VS1.C]: any
// CHECK-NEXT:  %19 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %20 = UnionNarrowTrustedInst (:classInstance()) %19: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %20: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %20: classInstance(), %17: object, "prototype": string
// CHECK-NEXT:        BranchInst %BB2
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function C(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -O0 -fno-inline -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class C {
  override(): number {
    return 1;
  }
  override2(x: number): number|string {
    return 1;
  }
}

class D extends C {
  constructor() {
    'use strict'
    super();
  }
  override(): number {
    return 2;
  }
  override2(x: number|string): number {
    return 2;
  }
}

function foo(c: C, d: D){
  c.override();
  d.override();
}

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

// CHECK:scope %VS1 [exports: any, C: any, D: any, foo: any, ?C.prototype: classInstance(), ?D.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.D]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:function(any)) %1: environment, %VS1: any, %foo(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: function(any), [%VS1.foo]: any
// CHECK-NEXT:  %8 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %8: object, [%VS1.C]: any
// CHECK-NEXT:  %10 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %override(): functionCode
// CHECK-NEXT:  %11 = CreateFunctionInst (:function(string|number)) %1: environment, %VS1: any, %override2(): functionCode
// CHECK-NEXT:  %12 = AllocTypedNonEnumObjectInst (:object) null: null, "override": string, %10: function(number), "override2": string, %11: function(string|number)
// CHECK-NEXT:  %13 = UnionNarrowTrustedInst (:classInstance()) %12: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %13: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %13: classInstance(), %8: object, "prototype": string
// CHECK-NEXT:  %16 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %17 = CheckedTypeCastInst (:object) %16: any, type(object)
// CHECK-NEXT:  %18 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %D(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %18: function(undefined), [%VS1.D]: any
// CHECK-NEXT:  %20 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %21 = UnionNarrowTrustedInst (:object) %20: classInstance()
// CHECK-NEXT:  %22 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %"override 1#"(): functionCode
// CHECK-NEXT:  %23 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %"override2 1#"(): functionCode
// CHECK-NEXT:  %24 = AllocTypedNonEnumObjectInst (:object) %21: object, "override": string, %22: function(number), "override2": string, %23: function(number)
// CHECK-NEXT:  %25 = UnionNarrowTrustedInst (:classInstance()) %24: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %25: classInstance(), [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %25: classInstance(), %18: function(undefined), "prototype": string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [c: any, d: any]

// CHECK:function foo(c: classInstance(C), d: classInstance(D)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(C)) %c: classInstance(C)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(C), [%VS2.c]: any
// CHECK-NEXT:  %4 = LoadParamInst (:classInstance(D)) %d: classInstance(D)
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: classInstance(D), [%VS2.d]: any
// CHECK-NEXT:  %6 = LoadFrameInst (:any) %1: environment, [%VS2.c]: any
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:classInstance(C)) %6: any, type(classInstance(C))
// CHECK-NEXT:  %8 = TypedLoadParentInst (:object) %7: classInstance(C)
// CHECK-NEXT:  %9 = PrLoadInst (:function(number)) %8: object, 0: number, "override": string
// CHECK-NEXT:  %10 = CallInst [njsf] (:number) %9: function(number), empty: any, false: boolean, empty: any, undefined: undefined, %7: classInstance(C)
// CHECK-NEXT:  %11 = LoadFrameInst (:any) %1: environment, [%VS2.d]: any
// CHECK-NEXT:  %12 = CheckedTypeCastInst (:classInstance(D)) %11: any, type(classInstance(D))
// CHECK-NEXT:  %13 = TypedLoadParentInst (:object) %12: classInstance(D)
// CHECK-NEXT:  %14 = PrLoadInst (:function(number)) %13: object, 0: number, "override": string
// CHECK-NEXT:  %15 = CallInst [njsf] (:number) %14: function(number), %"override 1#"(): functionCode, true: boolean, empty: any, undefined: undefined, %12: classInstance(D)
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function C(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS4 []

// CHECK:function override(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

// CHECK:scope %VS5 [x: any]

// CHECK:function override2(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS5.x]: any
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

// CHECK:scope %VS6 []

// CHECK:derived constructor D(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(D)) %<this>: classInstance(D)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS6: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %4 = CheckedTypeCastInst (:object) %3: any, type(object)
// CHECK-NEXT:  %5 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %6 = CallInst (:any) %4: object, empty: any, false: boolean, empty: any, %5: object, %0: classInstance(D)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS7 []

// CHECK:function "override 1#"(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS7: any, %0: environment
// CHECK-NEXT:       ReturnInst 2: number
// CHECK-NEXT:function_end

// CHECK:scope %VS8 [x: any]

// CHECK:function "override2 1#"(x: string|number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS8: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:string|number) %x: string|number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: string|number, [%VS8.x]: any
// CHECK-NEXT:       ReturnInst 2: number
// CHECK-NEXT:function_end

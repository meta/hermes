/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -O0 -Werror -typed -dump-ir %s | %FileCheckOrRegen %s --match-full-lines

class C {
  #x: number = 1;
  y: number;
  constructor(x, y) {
    this.#x = x;
    this.y = y;
  }

  method(): number {
    return this.#privateMethod();
  }

  #privateMethod(): number {
    return this.#x;
  }
}

new C(1, 2);

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

// CHECK:scope %VS1 [exports: any, C: any, #privateMethod: empty|object, <fieldInitFuncVar:C>: object, ?C.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, empty: empty, [%VS1.#privateMethod]: empty|object
// CHECK-NEXT:  %6 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %#privateMethod(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: function(number), [%VS1.#privateMethod]: empty|object
// CHECK-NEXT:  %8 = CreateFunctionInst (:object) %1: environment, %VS1: any, %<instance_members_initializer:C>(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %8: object, [%VS1.<fieldInitFuncVar:C>]: object
// CHECK-NEXT:  %10 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: function(undefined), [%VS1.C]: any
// CHECK-NEXT:  %12 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %method(): functionCode
// CHECK-NEXT:  %13 = AllocTypedNonEnumObjectInst (:object) null: null, "method": string, %12: function(number)
// CHECK-NEXT:  %14 = UnionNarrowTrustedInst (:classInstance()) %13: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %14: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %14: classInstance(), %10: function(undefined), "prototype": string
// CHECK-NEXT:  %17 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %18 = CheckedTypeCastInst (:function(undefined)) %17: any, type(function(undefined))
// CHECK-NEXT:  %19 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %20 = AllocTypedObjectInst (:object) %19: classInstance(), private "#x": any, 0: number, "y": string, 0: number
// CHECK-NEXT:  %21 = UnionNarrowTrustedInst (:classInstance(C)) %20: object
// CHECK-NEXT:  %22 = CallInst (:any) %18: function(undefined), %C(): functionCode, true: boolean, empty: any, %18: function(undefined), %21: classInstance(C), 1: number, 2: number
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function #privateMethod(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C)) %<this>: classInstance(C)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS2: any, %1: environment
// CHECK-NEXT:  %3 = PrLoadInst (:number) %0: classInstance(C), 0: number, private "#x": any
// CHECK-NEXT:       ReturnInst %3: number
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function <instance_members_initializer:C>(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C)) %<this>: classInstance(C)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS3: any, %1: environment
// CHECK-NEXT:       PrStoreInst 1: number, %0: classInstance(C), 0: number, private "#x": any, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [x: any, y: any]

// CHECK:base constructor C(x: any, y: any): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C)) %<this>: classInstance(C)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS4: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:any) %x: any
// CHECK-NEXT:       StoreFrameInst %2: environment, %3: any, [%VS4.x]: any
// CHECK-NEXT:  %5 = LoadParamInst (:any) %y: any
// CHECK-NEXT:       StoreFrameInst %2: environment, %5: any, [%VS4.y]: any
// CHECK-NEXT:  %7 = LoadFrameInst (:object) %1: environment, [%VS1.<fieldInitFuncVar:C>]: object
// CHECK-NEXT:  %8 = CallInst (:undefined) %7: object, %<instance_members_initializer:C>(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(C)
// CHECK-NEXT:  %9 = LoadFrameInst (:any) %2: environment, [%VS4.x]: any
// CHECK-NEXT:  %10 = CheckedTypeCastInst (:number) %9: any, type(number)
// CHECK-NEXT:        PrStoreInst %10: number, %0: classInstance(C), 0: number, private "#x": any, true: boolean
// CHECK-NEXT:  %12 = LoadFrameInst (:any) %2: environment, [%VS4.y]: any
// CHECK-NEXT:  %13 = CheckedTypeCastInst (:number) %12: any, type(number)
// CHECK-NEXT:        PrStoreInst %13: number, %0: classInstance(C), 1: number, "y": string, true: boolean
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 []

// CHECK:function method(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C)) %<this>: classInstance(C)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS5: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:empty|object) %1: environment, [%VS1.#privateMethod]: empty|object
// CHECK-NEXT:  %4 = ThrowIfInst (:object) %3: empty|object, type(empty)
// CHECK-NEXT:  %5 = CallInst [njsf] (:number) %4: object, %#privateMethod(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(C)
// CHECK-NEXT:       ReturnInst %5: number
// CHECK-NEXT:function_end

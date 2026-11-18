/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -O0 -typed -dump-ir %s | %FileCheckOrRegen %s --match-full-lines

class Box {
  value: number;

  constructor() {
    this.value = 0;
  }

  @Hermes.final
  identity<T>(x: T): T {
    return x;
  }
}

let box: Box = new Box();

// Test explicit type arguments.
let a: number = box.identity<number>(42);
let b: string = box.identity<string>("hello");

// Test type inference.
let c: number = box.identity(42);
let d: string = box.identity("hello");

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

// CHECK:scope %VS1 [exports: any, Box: any, identity: empty|object, identity#1: empty|object, box: any, a: any, b: any, c: any, d: any, ?Box.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.Box]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, empty: empty, [%VS1.identity]: empty|object
// CHECK-NEXT:       StoreFrameInst %1: environment, empty: empty, [%VS1.identity#1]: empty|object
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.box]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.a]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.b]: any
// CHECK-NEXT:        StoreFrameInst %1: environment, undefined: undefined, [%VS1.c]: any
// CHECK-NEXT:        StoreFrameInst %1: environment, undefined: undefined, [%VS1.d]: any
// CHECK-NEXT:  %12 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %Box(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %12: function(undefined), [%VS1.Box]: any
// CHECK-NEXT:  %14 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %15 = UnionNarrowTrustedInst (:classInstance()) %14: object
// CHECK-NEXT:  %16 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %identity(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %16: function(number), [%VS1.identity]: empty|object
// CHECK-NEXT:  %18 = CreateFunctionInst (:function(string)) %1: environment, %VS1: any, %"identity 1#"(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %18: function(string), [%VS1.identity#1]: empty|object
// CHECK-NEXT:        StoreFrameInst %1: environment, %15: classInstance(), [%VS1.?Box.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %15: classInstance(), %12: function(undefined), "prototype": string
// CHECK-NEXT:  %22 = LoadFrameInst (:any) %1: environment, [%VS1.Box]: any
// CHECK-NEXT:  %23 = CheckedTypeCastInst (:function(undefined)) %22: any, type(function(undefined))
// CHECK-NEXT:  %24 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?Box.prototype]: classInstance()
// CHECK-NEXT:  %25 = AllocTypedObjectInst (:object) %24: classInstance(), "value": string, 0: number
// CHECK-NEXT:  %26 = UnionNarrowTrustedInst (:classInstance(Box)) %25: object
// CHECK-NEXT:  %27 = CallInst (:any) %23: function(undefined), %Box(): functionCode, true: boolean, empty: any, %23: function(undefined), %26: classInstance(Box)
// CHECK-NEXT:        StoreFrameInst %1: environment, %26: classInstance(Box), [%VS1.box]: any
// CHECK-NEXT:  %29 = LoadFrameInst (:any) %1: environment, [%VS1.box]: any
// CHECK-NEXT:  %30 = CheckedTypeCastInst (:classInstance(Box)) %29: any, type(classInstance(Box))
// CHECK-NEXT:  %31 = LoadFrameInst (:empty|object) %1: environment, [%VS1.identity]: empty|object
// CHECK-NEXT:  %32 = ThrowIfInst (:object) %31: empty|object, type(empty)
// CHECK-NEXT:  %33 = CallInst [njsf] (:number) %32: object, %identity(): functionCode, true: boolean, empty: any, undefined: undefined, %30: classInstance(Box), 42: number
// CHECK-NEXT:        StoreFrameInst %1: environment, %33: number, [%VS1.a]: any
// CHECK-NEXT:  %35 = LoadFrameInst (:any) %1: environment, [%VS1.box]: any
// CHECK-NEXT:  %36 = CheckedTypeCastInst (:classInstance(Box)) %35: any, type(classInstance(Box))
// CHECK-NEXT:  %37 = LoadFrameInst (:empty|object) %1: environment, [%VS1.identity#1]: empty|object
// CHECK-NEXT:  %38 = ThrowIfInst (:object) %37: empty|object, type(empty)
// CHECK-NEXT:  %39 = CallInst [njsf] (:string) %38: object, %"identity 1#"(): functionCode, true: boolean, empty: any, undefined: undefined, %36: classInstance(Box), "hello": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %39: string, [%VS1.b]: any
// CHECK-NEXT:  %41 = LoadFrameInst (:any) %1: environment, [%VS1.box]: any
// CHECK-NEXT:  %42 = CheckedTypeCastInst (:classInstance(Box)) %41: any, type(classInstance(Box))
// CHECK-NEXT:  %43 = LoadFrameInst (:empty|object) %1: environment, [%VS1.identity]: empty|object
// CHECK-NEXT:  %44 = ThrowIfInst (:object) %43: empty|object, type(empty)
// CHECK-NEXT:  %45 = CallInst [njsf] (:number) %44: object, %identity(): functionCode, true: boolean, empty: any, undefined: undefined, %42: classInstance(Box), 42: number
// CHECK-NEXT:        StoreFrameInst %1: environment, %45: number, [%VS1.c]: any
// CHECK-NEXT:  %47 = LoadFrameInst (:any) %1: environment, [%VS1.box]: any
// CHECK-NEXT:  %48 = CheckedTypeCastInst (:classInstance(Box)) %47: any, type(classInstance(Box))
// CHECK-NEXT:  %49 = LoadFrameInst (:empty|object) %1: environment, [%VS1.identity#1]: empty|object
// CHECK-NEXT:  %50 = ThrowIfInst (:object) %49: empty|object, type(empty)
// CHECK-NEXT:  %51 = CallInst [njsf] (:string) %50: object, %"identity 1#"(): functionCode, true: boolean, empty: any, undefined: undefined, %48: classInstance(Box), "hello": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %51: string, [%VS1.d]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:base constructor Box(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Box)) %<this>: classInstance(Box)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS2: any, %1: environment
// CHECK-NEXT:       PrStoreInst 0: number, %0: classInstance(Box), 0: number, "value": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [x: any]

// CHECK:function identity(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS3.x]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS3.x]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:number) %4: any, type(number)
// CHECK-NEXT:       ReturnInst %5: number
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [x: any]

// CHECK:function "identity 1#"(x: string): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:string) %x: string
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: string, [%VS4.x]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS4.x]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:string) %4: any, type(string)
// CHECK-NEXT:       ReturnInst %5: string
// CHECK-NEXT:function_end

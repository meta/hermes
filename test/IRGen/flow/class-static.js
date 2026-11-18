/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -O0 -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

// Declared before the class, so the stores of the defaults are visibly hoisted
// above it while the static elements initializer stays at the declaration.
let before: number = 1;

class A {
  static x: number = 42;
  static foo(): number { return 1; }
  // 'this' is the class, so this reads the static field's variable rather than
  // a property off the receiver.
  static viaThis(): number { return this.x; }
  bar(): number { return 2; }
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

// CHECK:scope %VS1 [exports: any, before: any, A: any, x: number, foo: empty|object, viaThis: empty|object, ?A.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.before]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.A]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, 0: number, [%VS1.x]: number
// CHECK-NEXT:       StoreFrameInst %1: environment, empty: empty, [%VS1.foo]: empty|object
// CHECK-NEXT:       StoreFrameInst %1: environment, empty: empty, [%VS1.viaThis]: empty|object
// CHECK-NEXT:       StoreFrameInst %1: environment, 1: number, [%VS1.before]: any
// CHECK-NEXT:  %10 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %foo(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: function(number), [%VS1.foo]: empty|object
// CHECK-NEXT:  %12 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %viaThis(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %12: function(number), [%VS1.viaThis]: empty|object
// CHECK-NEXT:  %14 = CreateFunctionInst (:object) %1: environment, %VS1: any, %A(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %14: object, [%VS1.A]: any
// CHECK-NEXT:  %16 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %bar(): functionCode
// CHECK-NEXT:  %17 = AllocTypedNonEnumObjectInst (:object) null: null, "bar": string, %16: function(number)
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance()) %17: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %18: classInstance(), [%VS1.?A.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %18: classInstance(), %14: object, "prototype": string
// CHECK-NEXT:  %21 = CreateFunctionInst (:object) %1: environment, %VS1: any, %<static_elements_initializer:A>(): functionCode
// CHECK-NEXT:  %22 = CallInst (:undefined) %21: object, %<static_elements_initializer:A>(): functionCode, true: boolean, %1: environment, undefined: undefined, %14: object
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function foo(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function viaThis(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadFrameInst (:number) %0: environment, [%VS1.x]: number
// CHECK-NEXT:       ReturnInst %2: number
// CHECK-NEXT:function_end

// CHECK:scope %VS4 []

// CHECK:function A(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 []

// CHECK:function bar(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:       ReturnInst 2: number
// CHECK-NEXT:function_end

// CHECK:scope %VS6 []

// CHECK:function <static_elements_initializer:A>(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS6: any, %0: environment
// CHECK-NEXT:       StoreFrameInst %0: environment, 42: number, [%VS1.x]: number
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

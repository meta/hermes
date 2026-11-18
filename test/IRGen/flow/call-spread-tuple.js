/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O0 -fno-std-globals -typed -dump-ir %s | %FileCheckOrRegen %s --match-full-lines

'use strict';

function f(a: number, b: string): void {}

// A tuple spread is expanded into one PrLoadInst per element, followed by an
// ordinary direct call. No array is allocated and no builtin is invoked.
function spread(t: [number, string]): void {
  f(...t);
}

// Mixed with an ordinary argument.
function mixed(t: [string]): void {
  f(1, ...t);
}

// An empty tuple still evaluates its source, but contributes no arguments.
function mk(): [] {
  return [];
}
function emptyTuple(t: [number, string]): void {
  f(...t, ...mk());
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

// CHECK:scope %VS1 [exports: any, f: any, spread: any, mixed: any, mk: any, emptyTuple: any]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:  %4 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %f(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: function(undefined), [%VS1.f]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %spread(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: function(undefined), [%VS1.spread]: any
// CHECK-NEXT:  %8 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %mixed(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %8: function(undefined), [%VS1.mixed]: any
// CHECK-NEXT:  %10 = CreateFunctionInst (:function(tuple())) %1: environment, %VS1: any, %mk(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: function(tuple()), [%VS1.mk]: any
// CHECK-NEXT:  %12 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %emptyTuple(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %12: function(undefined), [%VS1.emptyTuple]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [a: any, b: any]

// CHECK:function f(a: number, b: string): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %a: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS2.a]: any
// CHECK-NEXT:  %4 = LoadParamInst (:string) %b: string
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: string, [%VS2.b]: any
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [t: any]

// CHECK:function spread(t: tuple(number, string)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(number, string)) %t: tuple(number, string)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(number, string), [%VS3.t]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %0: environment, [%VS1.f]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:function(undefined)) %4: any, type(function(undefined))
// CHECK-NEXT:  %6 = LoadFrameInst (:any) %1: environment, [%VS3.t]: any
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:tuple(number, string)) %6: any, type(tuple(number, string))
// CHECK-NEXT:  %8 = PrLoadInst (:number) %7: tuple(number, string), 0: number, "0": string
// CHECK-NEXT:  %9 = PrLoadInst (:string) %7: tuple(number, string), 1: number, "1": string
// CHECK-NEXT:  %10 = CallInst [njsf] (:undefined) %5: function(undefined), empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %8: number, %9: string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [t: any]

// CHECK:function mixed(t: tuple(string)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(string)) %t: tuple(string)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(string), [%VS4.t]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %0: environment, [%VS1.f]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:function(undefined)) %4: any, type(function(undefined))
// CHECK-NEXT:  %6 = LoadFrameInst (:any) %1: environment, [%VS4.t]: any
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:tuple(string)) %6: any, type(tuple(string))
// CHECK-NEXT:  %8 = PrLoadInst (:string) %7: tuple(string), 0: number, "0": string
// CHECK-NEXT:  %9 = CallInst [njsf] (:undefined) %5: function(undefined), empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, 1: number, %8: string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 []

// CHECK:function mk(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:  %2 = AllocTypedObjectInst (:object) empty: any
// CHECK-NEXT:  %3 = UnionNarrowTrustedInst (:tuple()) %2: object
// CHECK-NEXT:       ReturnInst %3: tuple()
// CHECK-NEXT:function_end

// CHECK:scope %VS6 [t: any]

// CHECK:function emptyTuple(t: tuple(number, string)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS6: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(number, string)) %t: tuple(number, string)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(number, string), [%VS6.t]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %0: environment, [%VS1.f]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:function(undefined)) %4: any, type(function(undefined))
// CHECK-NEXT:  %6 = LoadFrameInst (:any) %1: environment, [%VS6.t]: any
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:tuple(number, string)) %6: any, type(tuple(number, string))
// CHECK-NEXT:  %8 = PrLoadInst (:number) %7: tuple(number, string), 0: number, "0": string
// CHECK-NEXT:  %9 = PrLoadInst (:string) %7: tuple(number, string), 1: number, "1": string
// CHECK-NEXT:  %10 = LoadFrameInst (:any) %0: environment, [%VS1.mk]: any
// CHECK-NEXT:  %11 = CheckedTypeCastInst (:function(tuple())) %10: any, type(function(tuple()))
// CHECK-NEXT:  %12 = CallInst [njsf] (:tuple()) %11: function(tuple()), empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined
// CHECK-NEXT:  %13 = CallInst [njsf] (:undefined) %5: function(undefined), empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %8: number, %9: string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

// Test the code generation for some basic array operations.

function foo(x: number[], sink: any): void {
sink(x[0]);
sink(x.push(3.14));
x[3] = 42;
for(var i = 0, e = x.length; i < e; i++)
  sink(x[i]);
}

return foo;

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [length: empty|function(number)]

// CHECK:function global(): function(undefined)
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:       StoreFrameInst %0: environment, empty: empty, [%VS0.length]: empty|function(number)
// CHECK-NEXT:  %2 = CreateFunctionInst (:object) empty: any, empty: any, %Array(): functionCode
// CHECK-NEXT:  %3 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:classInstance()) %3: object
// CHECK-NEXT:       StorePropertyStrictInst %4: classInstance(), %2: object, "prototype": string
// CHECK-NEXT:  %6 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 1#"(): functionCode
// CHECK-NEXT:  %7 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %8 = UnionNarrowTrustedInst (:classInstance()) %7: object
// CHECK-NEXT:       StorePropertyStrictInst %8: classInstance(), %6: object, "prototype": string
// CHECK-NEXT:  %10 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 2#"(): functionCode
// CHECK-NEXT:  %11 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %12 = UnionNarrowTrustedInst (:classInstance()) %11: object
// CHECK-NEXT:        StorePropertyStrictInst %12: classInstance(), %10: object, "prototype": string
// CHECK-NEXT:  %14 = CreateFunctionInst (:function(number)) empty: any, empty: any, %"length 3#"(): functionCode
// CHECK-NEXT:        StoreFrameInst %0: environment, %14: function(number), [%VS0.length]: empty|function(number)
// CHECK-NEXT:  %16 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 3#"(): functionCode
// CHECK-NEXT:  %17 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance()) %17: object
// CHECK-NEXT:        StorePropertyStrictInst %18: classInstance(), %16: object, "prototype": string
// CHECK-NEXT:  %20 = CreateFunctionInst (:function(undefined)) empty: any, empty: any, %Symbol(): functionCode
// CHECK-NEXT:  %21 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %22 = UnionNarrowTrustedInst (:classInstance()) %21: object
// CHECK-NEXT:        StorePropertyStrictInst %22: classInstance(), %20: function(undefined), "prototype": string
// CHECK-NEXT:  %24 = CreateFunctionInst (:object) empty: any, empty: any, %Object(): functionCode
// CHECK-NEXT:  %25 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %26 = UnionNarrowTrustedInst (:classInstance()) %25: object
// CHECK-NEXT:        StorePropertyStrictInst %26: classInstance(), %24: object, "prototype": string
// CHECK-NEXT:  %28 = CreateFunctionInst (:function(undefined)) %0: environment, %VS0: any, %foo(): functionCode
// CHECK-NEXT:        ReturnInst %28: function(undefined)
// CHECK-NEXT:function_end

// CHECK:function Array(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 1#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 2#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "length 3#"(): number [allCallsitesKnownInStrictMode,typed,unreachable]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       UnreachableInst
// CHECK-NEXT:function_end

// CHECK:function "Array 3#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor Symbol(): any [typed,noReturn]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = TryLoadGlobalPropertyInst (:any) globalObject: object, "TypeError": string
// CHECK-NEXT:  %1 = CreateThisInst (:undefined|object) %0: any, %0: any, empty: any
// CHECK-NEXT:  %2 = CallInst (:any) %0: any, empty: any, false: boolean, empty: any, %0: any, %1: undefined|object, "Symbol is not a constructor": string
// CHECK-NEXT:  %3 = GetConstructedObjectInst (:object) %1: undefined|object, %2: any
// CHECK-NEXT:       ThrowInst %3: object
// CHECK-NEXT:function_end

// CHECK:function Object(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function foo(x: array(number), sink: any): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:array(number)) %x: array(number)
// CHECK-NEXT:  %2 = LoadParamInst (:any) %sink: any
// CHECK-NEXT:  %3 = FastArrayLoadInst (:number) %1: array(number), 0: number
// CHECK-NEXT:  %4 = CallInst (:any) %2: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %3: number
// CHECK-NEXT:       FastArrayPushInst 3.14: number, %1: array(number)
// CHECK-NEXT:  %6 = FastArrayLengthInst (:number) %1: array(number)
// CHECK-NEXT:  %7 = CallInst (:any) %2: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %6: number
// CHECK-NEXT:       FastArrayStoreInst 42: number, %1: array(number), 3: number
// CHECK-NEXT:  %9 = LoadFrameInst (:empty|function(number)) %0: environment, [%VS0.length]: empty|function(number)
// CHECK-NEXT:  %10 = ThrowIfInst (:function(number)) %9: empty|function(number), type(empty)
// CHECK-NEXT:  %11 = FastArrayLengthInst (:number) %1: array(number)
// CHECK-NEXT:  %12 = FLessThanInst (:boolean) 0: number, %11: number
// CHECK-NEXT:        CondBranchInst %12: boolean, %BB1, %BB2
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %14 = PhiInst (:number) 0: number, %BB0, %17: number, %BB1
// CHECK-NEXT:  %15 = FastArrayLoadInst (:number) %1: array(number), %14: number
// CHECK-NEXT:  %16 = CallInst (:any) %2: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %15: number
// CHECK-NEXT:  %17 = FAddInst (:number) %14: number, 1: number
// CHECK-NEXT:  %18 = FLessThanInst (:boolean) %17: number, %11: number
// CHECK-NEXT:        CondBranchInst %18: boolean, %BB1, %BB2
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

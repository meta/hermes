/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=main -typed -O -dump-ir %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %hermes -Xdump-functions=main -typed -O -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

// join() coerces every element with __ToString, which uses AddEmptyStringInst
// for primitives and only calls the global String() for objects. Requires -O
// so join() and __ToString are inlined into main.
return function main(x: number[]): string {
  return x.join("-");
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [join: empty|object, length: empty|object]

// CHECK:function main(x: classInstance(Array)): string [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:classInstance(Array)) %x: classInstance(Array)
// CHECK-NEXT:  %2 = LoadFrameInst (:empty|object) %0: environment, [%VS0.join]: empty|object
// CHECK-NEXT:  %3 = ThrowIfInst (:object) %2: empty|object, type(empty)
// CHECK-NEXT:  %4 = LoadFrameInst (:empty|object) %0: environment, [%VS0.length]: empty|object
// CHECK-NEXT:  %5 = ThrowIfInst (:object) %4: empty|object, type(empty)
// CHECK-NEXT:  %6 = FastArrayLengthInst (:number) %1: classInstance(Array)
// CHECK-NEXT:  %7 = FEqualInst (:boolean) %6: number, 0: number
// CHECK-NEXT:       CondBranchInst %7: boolean, %BB1, %BB2
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %9 = PhiInst (:string) %21: string, %BB6, "": string, %BB0
// CHECK-NEXT:        ReturnInst %9: string
// CHECK-NEXT:%BB2:
// CHECK-NEXT:  %11 = FastArrayLoadInst (:number) %1: classInstance(Array), 0: number
// CHECK-NEXT:  %12 = TypeOfIsInst (:boolean) %11: number, typeOfIs(Object,Null)
// CHECK-NEXT:        CondBranchInst %12: boolean, %BB12, %BB3
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %14 = TypeOfIsInst (:boolean) %11: number, typeOfIs(Function)
// CHECK-NEXT:        CondBranchInst %14: boolean, %BB12, %BB4
// CHECK-NEXT:%BB4:
// CHECK-NEXT:  %16 = AddEmptyStringInst (:string) %11: number
// CHECK-NEXT:        BranchInst %BB5
// CHECK-NEXT:%BB5:
// CHECK-NEXT:  %18 = PhiInst (:string) %16: string, %BB4, %43: string, %BB12
// CHECK-NEXT:  %19 = FLessThanInst (:boolean) 1: number, %6: number
// CHECK-NEXT:        CondBranchInst %19: boolean, %BB7, %BB6
// CHECK-NEXT:%BB6:
// CHECK-NEXT:  %21 = PhiInst (:string) %18: string, %BB5, %33: string, %BB10
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB7:
// CHECK-NEXT:  %23 = PhiInst (:number) 1: number, %BB5, %34: number, %BB10
// CHECK-NEXT:  %24 = PhiInst (:string) %18: string, %BB5, %33: string, %BB10
// CHECK-NEXT:  %25 = FastArrayLoadInst (:number) %1: classInstance(Array), %23: number
// CHECK-NEXT:  %26 = TypeOfIsInst (:boolean) %25: number, typeOfIs(Object,Null)
// CHECK-NEXT:        CondBranchInst %26: boolean, %BB11, %BB8
// CHECK-NEXT:%BB8:
// CHECK-NEXT:  %28 = TypeOfIsInst (:boolean) %25: number, typeOfIs(Function)
// CHECK-NEXT:        CondBranchInst %28: boolean, %BB11, %BB9
// CHECK-NEXT:%BB9:
// CHECK-NEXT:  %30 = AddEmptyStringInst (:string) %25: number
// CHECK-NEXT:        BranchInst %BB10
// CHECK-NEXT:%BB10:
// CHECK-NEXT:  %32 = PhiInst (:string) %30: string, %BB9, %39: string, %BB11
// CHECK-NEXT:  %33 = StringConcatInst (:string) %24: string, "-": string, %32: string
// CHECK-NEXT:  %34 = FAddInst (:number) %23: number, 1: number
// CHECK-NEXT:  %35 = FLessThanInst (:boolean) %34: number, %6: number
// CHECK-NEXT:        CondBranchInst %35: boolean, %BB7, %BB6
// CHECK-NEXT:%BB11:
// CHECK-NEXT:  %37 = TryLoadGlobalPropertyInst (:any) globalObject: object, "String": string
// CHECK-NEXT:  %38 = CallInst (:any) %37: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %25: number
// CHECK-NEXT:  %39 = CheckedTypeCastInst (:string) %38: any, type(string)
// CHECK-NEXT:        BranchInst %BB10
// CHECK-NEXT:%BB12:
// CHECK-NEXT:  %41 = TryLoadGlobalPropertyInst (:any) globalObject: object, "String": string
// CHECK-NEXT:  %42 = CallInst (:any) %41: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %11: number
// CHECK-NEXT:  %43 = CheckedTypeCastInst (:string) %42: any, type(string)
// CHECK-NEXT:        BranchInst %BB5
// CHECK-NEXT:function_end

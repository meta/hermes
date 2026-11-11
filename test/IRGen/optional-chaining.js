/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O0 -dump-ir %s | %FileCheckOrRegen %s --match-full-lines

function f1(a) {
  return a?.b;
}

function f2(f) {
  return f?.();
}

function f3(a) {
  return a?.b.c;
}

function f4(a) {
  return a?.b().c;
}

function f5(a) {
  return a?.().b;
}

function f6(a) {
  return (a?.b.c).d;
}

function f7(a) {
  return a?.b?.().c;
}

function f8(a) {
  return a?.b?.c?.();
}

function f9(a) {
  return delete a?.b;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:       DeclareGlobalVarInst "f1": string
// CHECK-NEXT:       DeclareGlobalVarInst "f2": string
// CHECK-NEXT:       DeclareGlobalVarInst "f3": string
// CHECK-NEXT:       DeclareGlobalVarInst "f4": string
// CHECK-NEXT:       DeclareGlobalVarInst "f5": string
// CHECK-NEXT:       DeclareGlobalVarInst "f6": string
// CHECK-NEXT:       DeclareGlobalVarInst "f7": string
// CHECK-NEXT:       DeclareGlobalVarInst "f8": string
// CHECK-NEXT:       DeclareGlobalVarInst "f9": string
// CHECK-NEXT:  %10 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f1(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %10: object, globalObject: object, "f1": string
// CHECK-NEXT:  %12 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f2(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %12: object, globalObject: object, "f2": string
// CHECK-NEXT:  %14 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f3(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %14: object, globalObject: object, "f3": string
// CHECK-NEXT:  %16 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f4(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %16: object, globalObject: object, "f4": string
// CHECK-NEXT:  %18 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f5(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %18: object, globalObject: object, "f5": string
// CHECK-NEXT:  %20 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f6(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %20: object, globalObject: object, "f6": string
// CHECK-NEXT:  %22 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f7(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %22: object, globalObject: object, "f7": string
// CHECK-NEXT:  %24 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f8(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %24: object, globalObject: object, "f8": string
// CHECK-NEXT:  %26 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f9(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %26: object, globalObject: object, "f9": string
// CHECK-NEXT:  %28 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:        StoreStackInst undefined: undefined, %28: any
// CHECK-NEXT:  %30 = LoadStackInst (:any) %28: any
// CHECK-NEXT:        ReturnInst %30: any
// CHECK-NEXT:function_end

// CHECK:scope %VS1 [a: any]

// CHECK:function f1(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS1.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %11: any, %BB3
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %4: any, %BB3
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %11 = LoadPropertyInst (:any) %4: any, "b": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [f: any]

// CHECK:function f2(f: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %f: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS2.f]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS2.f]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %10: any, %BB3
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:       BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %10 = CallInst (:any) %4: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [a: any]

// CHECK:function f3(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS3.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS3.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %12: any, %BB3
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %11: any, %BB3
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %11 = LoadPropertyInst (:any) %4: any, "b": string
// CHECK-NEXT:  %12 = LoadPropertyInst (:any) %11: any, "c": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [a: any]

// CHECK:function f4(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS4.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS4.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %13: any, %BB3
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %12: any, %BB3
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %11 = LoadPropertyInst (:any) %4: any, "b": string
// CHECK-NEXT:  %12 = CallInst (:any) %11: any, empty: any, false: boolean, empty: any, undefined: undefined, %4: any
// CHECK-NEXT:  %13 = LoadPropertyInst (:any) %12: any, "c": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS5 [a: any]

// CHECK:function f5(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS5.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS5.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %12: any, %BB3
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %11: any, %BB3
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %11 = CallInst (:any) %4: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined
// CHECK-NEXT:  %12 = LoadPropertyInst (:any) %11: any, "b": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS6 [a: any]

// CHECK:function f6(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS6: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS6.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS6.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %13: any, %BB3
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %12: any, %BB3
// CHECK-NEXT:  %9 = LoadPropertyInst (:any) %7: any, "d": string
// CHECK-NEXT:        ReturnInst %9: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %12 = LoadPropertyInst (:any) %4: any, "b": string
// CHECK-NEXT:  %13 = LoadPropertyInst (:any) %12: any, "c": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS7 [a: any]

// CHECK:function f7(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS7: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS7.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS7.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %15: any, %BB4
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %14: any, %BB4
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %11 = LoadPropertyInst (:any) %4: any, "b": string
// CHECK-NEXT:  %12 = BinaryEqualInst (:any) %11: any, null: null
// CHECK-NEXT:        CondBranchInst %12: any, %BB2, %BB4
// CHECK-NEXT:%BB4:
// CHECK-NEXT:  %14 = CallInst (:any) %11: any, empty: any, false: boolean, empty: any, undefined: undefined, %4: any
// CHECK-NEXT:  %15 = LoadPropertyInst (:any) %14: any, "c": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS8 [a: any]

// CHECK:function f8(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS8: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS8.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS8.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) undefined: undefined, %BB2, %16: any, %BB5
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:       BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %10 = LoadPropertyInst (:any) %4: any, "b": string
// CHECK-NEXT:  %11 = BinaryEqualInst (:any) %10: any, null: null
// CHECK-NEXT:        CondBranchInst %11: any, %BB2, %BB4
// CHECK-NEXT:%BB4:
// CHECK-NEXT:  %13 = LoadPropertyInst (:any) %10: any, "c": string
// CHECK-NEXT:  %14 = BinaryEqualInst (:any) %13: any, null: null
// CHECK-NEXT:        CondBranchInst %14: any, %BB2, %BB5
// CHECK-NEXT:%BB5:
// CHECK-NEXT:  %16 = CallInst (:any) %13: any, empty: any, false: boolean, empty: any, undefined: undefined, %10: any
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

// CHECK:scope %VS9 [a: any]

// CHECK:function f9(a: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS9: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %a: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS9.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS9.a]: any
// CHECK-NEXT:  %5 = BinaryEqualInst (:any) %4: any, null: null
// CHECK-NEXT:       CondBranchInst %5: any, %BB2, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %7 = PhiInst (:any) true: boolean, %BB2, %11: any, %BB3
// CHECK-NEXT:  %8 = PhiInst (:any) undefined: undefined, %BB2, %4: any, %BB3
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:%BB2:
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %11 = DeletePropertyLooseInst (:any) %4: any, "b": string
// CHECK-NEXT:        BranchInst %BB1
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermesc -O -dump-ir %s | %FileCheckOrRegen %s

// Test that loose equality comparisons against null|undefined, where the
// other operand's type is unknown, become TypeOfIs checks.

function eqNull(x) {
  return x == null;
}

function neqNull(x) {
  return x != null;
}

function eqUndefinedLeft(x) {
  return undefined == x;
}

function neqUndefinedLeft(x) {
  return void 0 != x;
}

function branch(x) {
  if (x == null)
    return 1;
  return 2;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:function global(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       DeclareGlobalVarInst "eqNull": string
// CHECK-NEXT:       DeclareGlobalVarInst "neqNull": string
// CHECK-NEXT:       DeclareGlobalVarInst "eqUndefinedLeft": string
// CHECK-NEXT:       DeclareGlobalVarInst "neqUndefinedLeft": string
// CHECK-NEXT:       DeclareGlobalVarInst "branch": string
// CHECK-NEXT:  %5 = CreateFunctionInst (:object) empty: any, empty: any, %eqNull(): functionCode
// CHECK-NEXT:       StorePropertyLooseInst %5: object, globalObject: object, "eqNull": string
// CHECK-NEXT:  %7 = CreateFunctionInst (:object) empty: any, empty: any, %neqNull(): functionCode
// CHECK-NEXT:       StorePropertyLooseInst %7: object, globalObject: object, "neqNull": string
// CHECK-NEXT:  %9 = CreateFunctionInst (:object) empty: any, empty: any, %eqUndefinedLeft(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %9: object, globalObject: object, "eqUndefinedLeft": string
// CHECK-NEXT:  %11 = CreateFunctionInst (:object) empty: any, empty: any, %neqUndefinedLeft(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %11: object, globalObject: object, "neqUndefinedLeft": string
// CHECK-NEXT:  %13 = CreateFunctionInst (:object) empty: any, empty: any, %branch(): functionCode
// CHECK-NEXT:        StorePropertyLooseInst %13: object, globalObject: object, "branch": string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function eqNull(x: any): boolean
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %x: any
// CHECK-NEXT:  %1 = TypeOfIsInst (:boolean) %0: any, typeOfIs(Undefined,Null)
// CHECK-NEXT:       ReturnInst %1: boolean
// CHECK-NEXT:function_end

// CHECK:function neqNull(x: any): boolean
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %x: any
// CHECK-NEXT:  %1 = TypeOfIsInst (:boolean) %0: any, typeOfIs(Object,String,Symbol,Boolean,Number,Bigint,Function)
// CHECK-NEXT:       ReturnInst %1: boolean
// CHECK-NEXT:function_end

// CHECK:function eqUndefinedLeft(x: any): boolean
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %x: any
// CHECK-NEXT:  %1 = TypeOfIsInst (:boolean) %0: any, typeOfIs(Undefined,Null)
// CHECK-NEXT:       ReturnInst %1: boolean
// CHECK-NEXT:function_end

// CHECK:function neqUndefinedLeft(x: any): boolean
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %x: any
// CHECK-NEXT:  %1 = TypeOfIsInst (:boolean) %0: any, typeOfIs(Object,String,Symbol,Boolean,Number,Bigint,Function)
// CHECK-NEXT:       ReturnInst %1: boolean
// CHECK-NEXT:function_end

// CHECK:function branch(x: any): number
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %x: any
// CHECK-NEXT:  %1 = TypeOfIsInst (:boolean) %0: any, typeOfIs(Undefined,Null)
// CHECK-NEXT:       CondBranchInst %1: boolean, %BB1, %BB2
// CHECK-NEXT:%BB1:
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:%BB2:
// CHECK-NEXT:       ReturnInst 2: number
// CHECK-NEXT:function_end

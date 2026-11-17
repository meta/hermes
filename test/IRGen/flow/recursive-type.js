/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=json,numStr,fself,gnull,firstFA,bself,firstA,firstK -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

// Recursive types whose cycle runs through an array or a function type.
// Lowering them must terminate, and so must dumping them: a cycle prints as a
// back-reference `^N` to the N-th enclosing array or function type.

(function () {

// Cycle through an array: number|string|array(<the same union>).
type Json = number | string | Json[];
function json(j: Json): Json {
  return j;
}

// A non-recursive array of a union prints the union's arms.
function numStr(xs: (number | string)[]): (number | string)[] {
  return xs;
}

// Cycle through a function's return type, with and without a union.
type F = (x: number) => F;
function fself(x: number): F {
  return fself;
}
type G = (x: number) => G | null;
function gnull(x: number): G | null {
  return x > 0 ? gnull : null;
}

// Cycle through a function's parameter and an array.
type FA = (x: FA[]) => number;
function firstFA(xs: FA[]): FA {
  return xs[0];
}

// Mutual recursion between an array alias and a function alias.
type A = B[];
type B = (a: A) => B;
function bself(a: A): B {
  return bself;
}
function firstA(a: A): B {
  return a[0];
}

// A class whose field is an array of functions returning the class.
class K {
  fs: (() => K)[];
  constructor() {
    this.fs = [];
  }
}
function firstK(k: K): () => K {
  return k.fs[0];
}

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [json: any, numStr: any, fself: any, gnull: any, firstFA: any, bself: any, firstA: any, K: any, firstK: any, ?K.prototype: classInstance()]

// CHECK:scope %VS1 [j: any]

// CHECK:function json(j: string|number|array(string|number|^1)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:string|number|array(string|number|^1)) %j: string|number|array(string|number|^1)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: string|number|array(string|number|^1), [%VS1.j]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS1.j]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:string|number|array(string|number|^1)) %4: any, type(string|number|array(string|number|^1))
// CHECK-NEXT:       ReturnInst %5: string|number|array(string|number|^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [xs: any]

// CHECK:function numStr(xs: array(string|number)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:array(string|number)) %xs: array(string|number)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: array(string|number), [%VS2.xs]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS2.xs]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:array(string|number)) %4: any, type(array(string|number))
// CHECK-NEXT:       ReturnInst %5: array(string|number)
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [x: any]

// CHECK:function fself(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS3.x]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %0: environment, [%VS0.fself]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:function(^1)) %4: any, type(function(^1))
// CHECK-NEXT:       ReturnInst %5: function(^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [x: any]

// CHECK:function gnull(x: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %x: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS4.x]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS4.x]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:number) %4: any, type(number)
// CHECK-NEXT:  %6 = BinaryGreaterThanInst (:boolean) %5: number, 0: number
// CHECK-NEXT:       CondBranchInst %6: boolean, %BB2, %BB1
// CHECK-NEXT:%BB1:
// CHECK-NEXT:       BranchInst %BB3
// CHECK-NEXT:%BB2:
// CHECK-NEXT:  %9 = LoadFrameInst (:any) %0: environment, [%VS0.gnull]: any
// CHECK-NEXT:  %10 = CheckedTypeCastInst (:function(null|^1)) %9: any, type(function(null|^1))
// CHECK-NEXT:        BranchInst %BB3
// CHECK-NEXT:%BB3:
// CHECK-NEXT:  %12 = PhiInst (:null|function(null|^1)) %10: function(null|^1), %BB2, null: null, %BB1
// CHECK-NEXT:        ReturnInst %12: null|function(null|^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS5 [xs: any]

// CHECK:function firstFA(xs: array(function(number))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:array(function(number))) %xs: array(function(number))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: array(function(number)), [%VS5.xs]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS5.xs]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:array(function(number))) %4: any, type(array(function(number)))
// CHECK-NEXT:  %6 = FastArrayLoadInst (:function(number)) %5: array(function(number)), 0: number
// CHECK-NEXT:       ReturnInst %6: function(number)
// CHECK-NEXT:function_end

// CHECK:scope %VS6 [a: any]

// CHECK:function bself(a: array(function(^1))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS6: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:array(function(^1))) %a: array(function(^1))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: array(function(^1)), [%VS6.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %0: environment, [%VS0.bself]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:function(^1)) %4: any, type(function(^1))
// CHECK-NEXT:       ReturnInst %5: function(^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS7 [a: any]

// CHECK:function firstA(a: array(function(^1))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS7: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:array(function(^1))) %a: array(function(^1))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: array(function(^1)), [%VS7.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS7.a]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:array(function(^1))) %4: any, type(array(function(^1)))
// CHECK-NEXT:  %6 = FastArrayLoadInst (:function(^1)) %5: array(function(^1)), 0: number
// CHECK-NEXT:       ReturnInst %6: function(^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS8 [k: any]

// CHECK:function firstK(k: classInstance(K)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS8: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(K)) %k: classInstance(K)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(K), [%VS8.k]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS8.k]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:classInstance(K)) %4: any, type(classInstance(K))
// CHECK-NEXT:  %6 = PrLoadInst (:uninit|array(function(classInstance(K)))) %5: classInstance(K), 0: number, "fs": string
// CHECK-NEXT:  %7 = ThrowIfInst (:array(function(classInstance(K)))) %6: uninit|array(function(classInstance(K))), type(uninit)
// CHECK-NEXT:  %8 = FastArrayLoadInst (:function(classInstance(K))) %7: array(function(classInstance(K))), 0: number
// CHECK-NEXT:       ReturnInst %8: function(classInstance(K))
// CHECK-NEXT:function_end

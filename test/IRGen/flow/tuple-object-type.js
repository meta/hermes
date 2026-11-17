/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=pair,obj,orderYX,tree,list,tf,ot,nested,dict -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

// Flow tuple and object types lower to IR tuple(...) and exactObject(...)
// types, including recursive ones. A cycle prints as a back-reference `^N` to
// the N-th enclosing structural type.

(function () {

// Tuple parameter and return.
function pair(p: [number, string]): [number, string] {
  return p;
}

// Object parameter and return.
function obj(o: {x: number, y: string}): {x: number, y: string} {
  return o;
}

// Field order is part of the type.
function orderYX(o: {y: string, x: number}): number {
  return o.x;
}

// Cycle through an object, under a union with null.
type Tree = null | {val: number, left: Tree, right: Tree};
function tree(t: Tree): Tree {
  return t;
}

// Cycle through a tuple.
type L = [number, L | null];
function list(l: L): L {
  return l;
}

// Cycle through a tuple and a function returning the tuple.
type TF = [number, () => TF];
function tf(t: TF): TF {
  return t;
}

// Mutual recursion between an object alias and a tuple alias.
type O = {t: T};
type T = [O | null];
function ot(o: O, t: T): O {
  return o;
}

// Cycle through a tuple and an array.
type N = [number, N[]];
function nested(n: N): N {
  return n;
}

// A cycle through an indexer is cut at the indexer, which lowers to object.
type D = {[string]: D};
function dict(d: D): D {
  return d;
}

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [pair: any, obj: any, orderYX: any, tree: any, list: any, tf: any, ot: any, nested: any, dict: any]

// CHECK:scope %VS1 [p: any]

// CHECK:function pair(p: tuple(number, string)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(number, string)) %p: tuple(number, string)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(number, string), [%VS1.p]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS1.p]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:tuple(number, string)) %4: any, type(tuple(number, string))
// CHECK-NEXT:       ReturnInst %5: tuple(number, string)
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [o: any]

// CHECK:function obj(o: exactObject(x: number, y: string)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(x: number, y: string)) %o: exactObject(x: number, y: string)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(x: number, y: string), [%VS2.o]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS2.o]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:exactObject(x: number, y: string)) %4: any, type(exactObject(x: number, y: string))
// CHECK-NEXT:       ReturnInst %5: exactObject(x: number, y: string)
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [o: any]

// CHECK:function orderYX(o: exactObject(y: string, x: number)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(y: string, x: number)) %o: exactObject(y: string, x: number)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(y: string, x: number), [%VS3.o]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS3.o]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:exactObject(y: string, x: number)) %4: any, type(exactObject(y: string, x: number))
// CHECK-NEXT:  %6 = PrLoadInst (:number) %5: exactObject(y: string, x: number), 1: number, "x": string
// CHECK-NEXT:       ReturnInst %6: number
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [t: any]

// CHECK:function tree(t: null|exactObject(val: number, left: null|^1, right: null|^1)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:null|exactObject(val: number, left: null|^1, right: null|^1)) %t: null|exactObject(val: number, left: null|^1, right: null|^1)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: null|exactObject(val: number, left: null|^1, right: null|^1), [%VS4.t]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS4.t]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:null|exactObject(val: number, left: null|^1, right: null|^1)) %4: any, type(null|exactObject(val: number, left: null|^1, right: null|^1))
// CHECK-NEXT:       ReturnInst %5: null|exactObject(val: number, left: null|^1, right: null|^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS5 [l: any]

// CHECK:function list(l: tuple(number, null|^1)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(number, null|^1)) %l: tuple(number, null|^1)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(number, null|^1), [%VS5.l]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS5.l]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:tuple(number, null|^1)) %4: any, type(tuple(number, null|^1))
// CHECK-NEXT:       ReturnInst %5: tuple(number, null|^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS6 [t: any]

// CHECK:function tf(t: tuple(number, function(^2))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS6: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(number, function(^2))) %t: tuple(number, function(^2))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(number, function(^2)), [%VS6.t]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS6.t]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:tuple(number, function(^2))) %4: any, type(tuple(number, function(^2)))
// CHECK-NEXT:       ReturnInst %5: tuple(number, function(^2))
// CHECK-NEXT:function_end

// CHECK:scope %VS7 [o: any, t: any]

// CHECK:function ot(o: exactObject(t: tuple(null|^2)), t: tuple(null|exactObject(t: ^2))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS7: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(t: tuple(null|^2))) %o: exactObject(t: tuple(null|^2))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(t: tuple(null|^2)), [%VS7.o]: any
// CHECK-NEXT:  %4 = LoadParamInst (:tuple(null|exactObject(t: ^2))) %t: tuple(null|exactObject(t: ^2))
// CHECK-NEXT:       StoreFrameInst %1: environment, %4: tuple(null|exactObject(t: ^2)), [%VS7.t]: any
// CHECK-NEXT:  %6 = LoadFrameInst (:any) %1: environment, [%VS7.o]: any
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:exactObject(t: tuple(null|^2))) %6: any, type(exactObject(t: tuple(null|^2)))
// CHECK-NEXT:       ReturnInst %7: exactObject(t: tuple(null|^2))
// CHECK-NEXT:function_end

// CHECK:scope %VS8 [n: any]

// CHECK:function nested(n: tuple(number, array(^2))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS8: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:tuple(number, array(^2))) %n: tuple(number, array(^2))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: tuple(number, array(^2)), [%VS8.n]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS8.n]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:tuple(number, array(^2))) %4: any, type(tuple(number, array(^2)))
// CHECK-NEXT:       ReturnInst %5: tuple(number, array(^2))
// CHECK-NEXT:function_end

// CHECK:scope %VS9 [d: any]

// CHECK:function dict(d: object): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS9: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:object) %d: object
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: object, [%VS9.d]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS9.d]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:object) %4: any, type(object)
// CHECK-NEXT:       ReturnInst %5: object
// CHECK-NEXT:function_end

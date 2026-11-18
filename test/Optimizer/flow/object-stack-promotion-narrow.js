/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=obj,tup,cls,escapes,chain,storeSelf,storeOther,storePropSelf,defineOther -typed -dump-ir -O %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s

// Typed object literals, tuple literals and class instances are allocated as
// plain object and narrowed (trusted) to their lowered type. Accesses through
// the narrow do not make the object escape, so a non-escaping one is still
// replaced by stack slots and its loads fold away. Passing the narrowed object
// to a call is an escape and keeps the allocation (escapes).

(function () {

class P {
  x: number;
  y: number;
  constructor(x: number, y: number) {
    this.x = x;
    this.y = y;
  }
}

function obj(n: number): number {
  'noinline';
  const o: {x: number, y: number} = {x: n, y: 2};
  return o.x + o.y;
}

function tup(n: number): number {
  'noinline';
  const t: [number, number] = [n, 2];
  return t[0] + t[1];
}

function cls(n: number): number {
  'noinline';
  const p: P = new P(n, 2);
  return p.x + p.y;
}

function sink(o: {x: number}): number {
  'noinline';
  return o.x;
}

function escapes(n: number): number {
  'noinline';
  const o: {x: number} = {x: n};
  return sink(o) + o.x;
}

// A chain of two trusted narrows. The literal is narrowed to its exact object
// type. Storing it into o2, whose field x is readonly with a wider type, narrows
// it again (trusted): under shallow subtyping neither exact object type is a
// subset of the other, so narrowToFlowType cannot cast. Both narrows survive
// until stack promotion, and the loads of o2 go through the second one, so
// promotion has to follow the chain from the allocation.
function chain(n: number): number {
  'noinline';
  const o1: {x: number, y: number} = {x: n, y: 2};
  const o2: {+x: number | string, y: number} = o1;
  const x: number | string = o2.x;
  return o2.y + (x === n ? 100 : 0);
}

// Storing the object as a value makes it escape, even when the stored value
// is one of its narrows rather than the allocation itself: into itself with a
// PrStoreInst (storeSelf) or a StorePropertyInst (storePropSelf), and into
// another object with a PrStoreInst (storeOther) or a DefineOwnPropertyInst
// (defineOther: after the computed key, the literal's properties are defined
// one by one, not allocated with the object). Each allocation of o is kept.
function storeSelf(n: number): number {
  'noinline';
  const o: {x: number, self: mixed} = {x: n, self: null};
  o.self = o;
  return o.x;
}

function storeOther(n: number): {o: {x: number}} {
  'noinline';
  const o: {x: number} = {x: n};
  return {o: o};
}

function storePropSelf(n: number): mixed {
  'noinline';
  const o: {x: mixed} = {x: n};
  const a: any = o;
  // Throws: typed object literals are frozen to untyped code.
  try {
    a.x = o;
  } catch (e) {}
  return o.x;
}

function defineOther(n: number, k: string): any {
  'noinline';
  const o: {x: number} = {x: n};
  return {[k]: 1, x: o};
}

print(obj(1), obj(10), tup(2), tup(20), cls(3), cls(30));
// EXEC: 3 12 4 22 5 32
print(escapes(4), escapes(40));
// EXEC-NEXT: 8 80
print(chain(1), chain(10));
// EXEC-NEXT: 102 102
print(storeSelf(3), storeOther(4).o.x, storePropSelf(5), defineOther(6, 'k').x.x);
// EXEC-NEXT: 3 4 5 6

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:function obj(n: number): number [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:number) %n: number
// CHECK-NEXT:  %1 = FAddInst (:number) %0: number, 2: number
// CHECK-NEXT:       ReturnInst %1: number
// CHECK-NEXT:function_end

// CHECK:function tup(n: number): number [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:number) %n: number
// CHECK-NEXT:  %1 = FAddInst (:number) %0: number, 2: number
// CHECK-NEXT:       ReturnInst %1: number
// CHECK-NEXT:function_end

// CHECK:scope %VS0 [sink: function(number), ?P.prototype: classInstance()]

// CHECK:function cls(n: number): number [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:number) %n: number
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?P.prototype]: classInstance()
// CHECK-NEXT:  %3 = FAddInst (:number) %1: number, 2: number
// CHECK-NEXT:       ReturnInst %3: number
// CHECK-NEXT:function_end

// CHECK:function escapes(n: number): number [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:number) %n: number
// CHECK-NEXT:  %2 = AllocTypedObjectInst (:object) empty: any, "x": string, %1: number
// CHECK-NEXT:  %3 = UnionNarrowTrustedInst (:exactObject(x: number)) %2: object
// CHECK-NEXT:  %4 = LoadFrameInst (:function(number)) %0: environment, [%VS0.sink]: function(number)
// CHECK-NEXT:  %5 = CallInst [njsf] (:number) %4: function(number), %sink(): functionCode, true: boolean, empty: any, undefined: undefined, 0: number, %3: exactObject(x: number)
// CHECK-NEXT:  %6 = PrLoadInst (:number) %3: exactObject(x: number), 0: number, "x": string
// CHECK-NEXT:  %7 = FAddInst (:number) %5: number, %6: number
// CHECK-NEXT:       ReturnInst %7: number
// CHECK-NEXT:function_end

// CHECK:function chain(n: number): number [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:number) %n: number
// CHECK-NEXT:  %1 = FEqualInst (:boolean) %0: number, %0: number
// CHECK-NEXT:       CondBranchInst %1: boolean, %BB1, %BB2
// CHECK-NEXT:%BB1:
// CHECK-NEXT:       BranchInst %BB2
// CHECK-NEXT:%BB2:
// CHECK-NEXT:  %4 = PhiInst (:number) 100: number, %BB1, 0: number, %BB0
// CHECK-NEXT:  %5 = FAddInst (:number) 2: number, %4: number
// CHECK-NEXT:       ReturnInst %5: number
// CHECK-NEXT:function_end

// CHECK:function storeSelf(n: number): number [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = AllocTypedObjectInst (:object) empty: any, "x": string, 3: number, "self": string, null: null
// CHECK-NEXT:  %1 = UnionNarrowTrustedInst (:exactObject(x: number, self: any)) %0: object
// CHECK-NEXT:       PrStoreInst %1: exactObject(x: number, self: any), %1: exactObject(x: number, self: any), 1: number, "self": string, false: boolean
// CHECK-NEXT:  %3 = PrLoadInst (:number) %1: exactObject(x: number, self: any), 0: number, "x": string
// CHECK-NEXT:       ReturnInst %3: number
// CHECK-NEXT:function_end

// CHECK:function storeOther(n: number): exactObject(o: exactObject(x: number)) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = AllocTypedObjectInst (:object) empty: any, "x": string, 4: number
// CHECK-NEXT:  %1 = UnionNarrowTrustedInst (:exactObject(x: number)) %0: object
// CHECK-NEXT:  %2 = AllocTypedObjectInst (:object) empty: any, "o": string, %1: exactObject(x: number)
// CHECK-NEXT:  %3 = UnionNarrowTrustedInst (:exactObject(o: exactObject(x: number))) %2: object
// CHECK-NEXT:       ReturnInst %3: exactObject(o: exactObject(x: number))
// CHECK-NEXT:function_end

// CHECK:function storePropSelf(n: number): any [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = AllocTypedObjectInst (:object) empty: any, "x": string, 5: number
// CHECK-NEXT:  %1 = UnionNarrowTrustedInst (:exactObject(x: any)) %0: object
// CHECK-NEXT:       TryStartInst %BB1, %BB3
// CHECK-NEXT:%BB1:
// CHECK-NEXT:  %3 = CatchInst (:any)
// CHECK-NEXT:       BranchInst %BB2
// CHECK-NEXT:%BB2:
// CHECK-NEXT:  %5 = PrLoadInst (:any) %1: exactObject(x: any), 0: number, "x": string
// CHECK-NEXT:       ReturnInst %5: any
// CHECK-NEXT:%BB3:
// CHECK-NEXT:       StorePropertyStrictInst %1: exactObject(x: any), %1: exactObject(x: any), "x": string
// CHECK-NEXT:       TryEndInst %BB1, %BB2
// CHECK-NEXT:function_end

// CHECK:function defineOther(n: number, k: string): object [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = AllocTypedObjectInst (:object) empty: any, "x": string, 6: number
// CHECK-NEXT:  %1 = UnionNarrowTrustedInst (:exactObject(x: number)) %0: object
// CHECK-NEXT:  %2 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:       DefineOwnPropertyInst 1: number, %2: object, "k": string, true: boolean
// CHECK-NEXT:       DefineOwnPropertyInst %1: exactObject(x: number), %2: object, "x": string, true: boolean
// CHECK-NEXT:       ReturnInst %2: object
// CHECK-NEXT:function_end

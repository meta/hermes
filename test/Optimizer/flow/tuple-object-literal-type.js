/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=mkTuple,mkObj,mkEmpty,rest -typed -dump-ir -O %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O0 -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s
// RUN: %shermes -typed -verify-ir -O -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s

// Typed tuple literals, object literals and object rest bindings are allocated
// as plain object and narrowed (trusted) to their lowered tuple(...) or
// exactObject(...) type, so returning them needs no runtime cast.

(function () {

function mkTuple(x: number): [number, string] {
  'noinline';
  return [x, 's'];
}

function mkObj(x: number): {x: number, y: string} {
  'noinline';
  return {x: x, y: 's'};
}

function mkEmpty(): {} {
  'noinline';
  return {};
}

function rest(o: {a: number, b: string, c: number}): {b: string, c: number} {
  'noinline';
  const {a, ...r} = o;
  return r;
}

print(mkTuple(1)[0], mkTuple(2)[1], mkObj(3).x, mkObj(4).y, typeof mkEmpty());
// EXEC: 1 s 3 s object
print(rest({a: 1, b: 'b', c: 2}).b, rest({a: 3, b: 'd', c: 4}).c);
// EXEC-NEXT: b 4

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:function mkTuple(x: number): tuple(number, string) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:number) %x: number
// CHECK-NEXT:  %1 = AllocObjectLiteralInst (:object) empty: any, "0": string, %0: number, "1": string, "s": string
// CHECK-NEXT:  %2 = UnionNarrowTrustedInst (:tuple(number, string)) %1: object
// CHECK-NEXT:       ReturnInst %2: tuple(number, string)
// CHECK-NEXT:function_end

// CHECK:function mkObj(x: number): exactObject(x: number, y: string) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:number) %x: number
// CHECK-NEXT:  %1 = AllocObjectLiteralInst (:object) empty: any, "x": string, 0: number, "y": string, "s": string
// CHECK-NEXT:       PrStoreInst %0: number, %1: object, 0: number, "x": string, true: boolean
// CHECK-NEXT:  %3 = UnionNarrowTrustedInst (:exactObject(x: number, y: string)) %1: object
// CHECK-NEXT:       ReturnInst %3: exactObject(x: number, y: string)
// CHECK-NEXT:function_end

// CHECK:function mkEmpty(): exactObject() [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:  %1 = UnionNarrowTrustedInst (:exactObject()) %0: object
// CHECK-NEXT:       ReturnInst %1: exactObject()
// CHECK-NEXT:function_end

// CHECK:function rest(o: exactObject(a: number, b: string, c: number)): exactObject(b: string, c: number) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:exactObject(a: number, b: string, c: number)) %o: exactObject(a: number, b: string, c: number)
// CHECK-NEXT:  %1 = AllocObjectLiteralInst (:object) empty: any, "b": string, "": string, "c": string, 0: number
// CHECK-NEXT:  %2 = PrLoadInst (:string) %0: exactObject(a: number, b: string, c: number), 1: number, "b": string
// CHECK-NEXT:       PrStoreInst %2: string, %1: object, 0: number, "b": string, false: boolean
// CHECK-NEXT:  %4 = PrLoadInst (:number) %0: exactObject(a: number, b: string, c: number), 2: number, "c": string
// CHECK-NEXT:       PrStoreInst %4: number, %1: object, 1: number, "c": string, true: boolean
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:exactObject(b: string, c: number)) %1: object
// CHECK-NEXT:       ReturnInst %6: exactObject(b: string, c: number)
// CHECK-NEXT:function_end

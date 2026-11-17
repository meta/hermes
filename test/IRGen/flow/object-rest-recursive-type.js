/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O0 -typed -Xdump-functions=decl,assign -dump-ir %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %shermes -O0 -typed -verify-ir -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s
// RUN: %shermes -O -typed -verify-ir -exec %s | %FileCheck --check-prefix=EXEC --match-full-lines %s

// The type of an object rest binding is the exact object of the remaining
// fields. Here it equals the recursive alias R, whose IR type is a reserved
// (non-interned) entry. The rest object is narrowed once, straight to R's IR
// type, in a declaration and in an assignment to a binding of type R.

(function () {

type R = {b: string, next: R | null};

function decl(o: {a: number, b: string, next: R | null}): R {
  const {a, ...r} = o;
  return r;
}

function assign(o: {a: number, b: string, next: R | null}): R {
  let a: number;
  let r: R;
  ({a, ...r} = o);
  return r;
}

const tail: R = {b: 'tail', next: null};
const d: R = decl({a: 1, b: 'd', next: tail});
const s: R = assign({a: 2, b: 's', next: null});
const dn = d.next;
print(d.b, dn === null ? 'null' : dn.b, s.b, s.next === null);
// EXEC: d tail s true

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [decl: any, assign: any, tail: any, d: any, s: any, dn: any]

// CHECK:scope %VS1 [o: any, a: any, r: any]

// CHECK:function decl(o: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))) %o: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), [%VS1.o]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.a]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.r]: any
// CHECK-NEXT:  %6 = LoadFrameInst (:any) %1: environment, [%VS1.o]: any
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))) %6: any, type(exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)))
// CHECK-NEXT:  %8 = PrLoadInst (:number) %7: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), 0: number, "a": string
// CHECK-NEXT:       StoreFrameInst %1: environment, %8: number, [%VS1.a]: any
// CHECK-NEXT:  %10 = AllocObjectLiteralInst (:object) empty: any, "b": string, "": string, "next": string, null: null
// CHECK-NEXT:  %11 = PrLoadInst (:string) %7: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), 1: number, "b": string
// CHECK-NEXT:        PrStoreInst %11: string, %10: object, 0: number, "b": string, false: boolean
// CHECK-NEXT:  %13 = PrLoadInst (:null|exactObject(b: string, next: null|^1)) %7: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), 2: number, "next": string
// CHECK-NEXT:        PrStoreInst %13: null|exactObject(b: string, next: null|^1), %10: object, 1: number, "next": string, false: boolean
// CHECK-NEXT:  %15 = UnionNarrowTrustedInst (:exactObject(b: string, next: null|^1)) %10: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %15: exactObject(b: string, next: null|^1), [%VS1.r]: any
// CHECK-NEXT:  %17 = LoadFrameInst (:any) %1: environment, [%VS1.r]: any
// CHECK-NEXT:  %18 = CheckedTypeCastInst (:exactObject(b: string, next: null|^1)) %17: any, type(exactObject(b: string, next: null|^1))
// CHECK-NEXT:        ReturnInst %18: exactObject(b: string, next: null|^1)
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [o: any, a: any, r: any]

// CHECK:function assign(o: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))) %o: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), [%VS2.o]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.a]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.r]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.a]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.r]: any
// CHECK-NEXT:  %8 = LoadFrameInst (:any) %1: environment, [%VS2.o]: any
// CHECK-NEXT:  %9 = CheckedTypeCastInst (:exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1))) %8: any, type(exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)))
// CHECK-NEXT:  %10 = PrLoadInst (:number) %9: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), 0: number, "a": string
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: number, [%VS2.a]: any
// CHECK-NEXT:  %12 = AllocObjectLiteralInst (:object) empty: any, "b": string, "": string, "next": string, null: null
// CHECK-NEXT:  %13 = PrLoadInst (:string) %9: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), 1: number, "b": string
// CHECK-NEXT:        PrStoreInst %13: string, %12: object, 0: number, "b": string, false: boolean
// CHECK-NEXT:  %15 = PrLoadInst (:null|exactObject(b: string, next: null|^1)) %9: exactObject(a: number, b: string, next: null|exactObject(b: string, next: null|^1)), 2: number, "next": string
// CHECK-NEXT:        PrStoreInst %15: null|exactObject(b: string, next: null|^1), %12: object, 1: number, "next": string, false: boolean
// CHECK-NEXT:  %17 = UnionNarrowTrustedInst (:exactObject(b: string, next: null|^1)) %12: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %17: exactObject(b: string, next: null|^1), [%VS2.r]: any
// CHECK-NEXT:  %19 = LoadFrameInst (:any) %1: environment, [%VS2.r]: any
// CHECK-NEXT:  %20 = CheckedTypeCastInst (:exactObject(b: string, next: null|^1)) %19: any, type(exactObject(b: string, next: null|^1))
// CHECK-NEXT:        ReturnInst %20: exactObject(b: string, next: null|^1)
// CHECK-NEXT:function_end

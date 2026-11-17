/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=getB,getA -typed -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

// Two classes whose fields reference each other. Lowering A lowers its field
// type B, which lowers its field type A again; the class lowering cache must
// hold every class whose lowering is in progress, not only the innermost one,
// or this does not terminate. Both field loads keep their class types.

(function () {

class A {
  b: B | null;
  constructor() {
    this.b = null;
  }
}

class B {
  a: A | null;
  constructor() {
    this.a = null;
  }
}

function getB(a: A): B | null {
  return a.b;
}

function getA(b: B): A | null {
  return b.a;
}

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [A: any, B: any, getB: any, getA: any, ?A.prototype: classInstance(), ?B.prototype: classInstance()]

// CHECK:scope %VS1 [a: any]

// CHECK:function getB(a: classInstance(A)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(A)) %a: classInstance(A)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(A), [%VS1.a]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS1.a]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:classInstance(A)) %4: any, type(classInstance(A))
// CHECK-NEXT:  %6 = PrLoadInst (:null|classInstance(B)) %5: classInstance(A), 0: number, "b": string
// CHECK-NEXT:       ReturnInst %6: null|classInstance(B)
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [b: any]

// CHECK:function getA(b: classInstance(B)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(B)) %b: classInstance(B)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(B), [%VS2.b]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS2.b]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:classInstance(B)) %4: any, type(classInstance(B))
// CHECK-NEXT:  %6 = PrLoadInst (:null|classInstance(A)) %5: classInstance(B), 0: number, "a": string
// CHECK-NEXT:       ReturnInst %6: null|classInstance(A)
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -typed -dump-ir -O0 -Xdump-functions=distinct,sub2,sub3,sub4,mix %s | %FileCheckOrRegen --match-full-lines %s

// Each specialization of a generic class is its own ClassInstance, and a class
// extending a specialization has that specialization as its IR superclass.
// Union normalization shows both: a subclass arm is subsumed by its superclass
// arm, and arms of unrelated classes are kept.

class A<T> {
  v: T;
  constructor(v: T) {
    this.v = v;
  }
}

class B1 extends A<number> {
  constructor(v: number) {
    super(v);
  }
}

class B2<T> extends A<T> {
  constructor(v: T) {
    super(v);
  }
}

class B3<T> extends A<number> {
  w: T;
  constructor(v: number, w: T) {
    super(v);
    this.w = w;
  }
}

class C2<T> extends B2<T> {
  constructor(v: T) {
    super(v);
  }
}

// Distinct specializations: two arms.
function distinct(x: A<number> | A<string>): void {}
// B2<string> extends A<string>: one arm.
function sub2(x: A<string> | B2<string>): void {}
// B1 and B3<string> extend A<number>: one arm.
function sub3(x: A<number> | B1 | B3<string>): void {}
// Two levels: C2<string> extends B2<string> extends A<string>: one arm.
function sub4(x: A<string> | C2<string>): void {}
// B2<string> does not extend A<number>: two arms.
function mix(x: A<number> | B2<string>): void {}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, B1: any, distinct: any, sub2: any, sub3: any, sub4: any, mix: any, A: any, A#1: any, B2: any, B3: any, C2: any, ?A.prototype: classInstance(), ?A.prototype#1: classInstance(), ?B1.prototype: classInstance(), ?B2.prototype: classInstance(), ?B3.prototype: classInstance(), ?C2.prototype: classInstance()]

// CHECK:scope %VS1 [x: any]

// CHECK:function distinct(x: classInstance(A)|classInstance(A)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(A)|classInstance(A)) %x: classInstance(A)|classInstance(A)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(A)|classInstance(A), [%VS1.x]: any
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [x: any]

// CHECK:function sub2(x: classInstance(A)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(A)) %x: classInstance(A)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(A), [%VS2.x]: any
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [x: any]

// CHECK:function sub3(x: classInstance(A)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(A)) %x: classInstance(A)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(A), [%VS3.x]: any
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS4 [x: any]

// CHECK:function sub4(x: classInstance(A)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(A)) %x: classInstance(A)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(A), [%VS4.x]: any
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 [x: any]

// CHECK:function mix(x: classInstance(A)|classInstance(B2)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:classInstance(A)|classInstance(B2)) %x: classInstance(A)|classInstance(B2)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: classInstance(A)|classInstance(B2), [%VS5.x]: any
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -typed -Xdump-functions=B,D -dump-ir -O0 %s | %FileCheckOrRegen --match-full-lines %s

// The implicit constructor of a derived class declares the parameters of
// the nearest explicit ancestor constructor. An optional parameter includes
// undefined in its type. A rest parameter gets no parameter of its own; all
// arguments are copied into an array and applied to the super constructor.

'use strict';

class A {
  constructor(x: number, y?: number) {}
}
class B extends A {}

class C {
  constructor(x: number, ...rest: number[]) {}
}
class D extends C {}

new B(1);
new D(1, 2, 3);

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, A: any, B: any, C: any, D: any, ?A.prototype: classInstance(), ?B.prototype: classInstance(), ?C.prototype: classInstance(), ?D.prototype: classInstance()]

// CHECK:scope %VS1 []

// CHECK:derived constructor B(x: number, y: undefined|number): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(B)) %<this>: classInstance(B)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS1: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:number) %x: number
// CHECK-NEXT:  %4 = LoadParamInst (:undefined|number) %y: undefined|number
// CHECK-NEXT:  %5 = LoadFrameInst (:any) %1: environment, [%VS0.A]: any
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:function(undefined)) %5: any, type(function(undefined))
// CHECK-NEXT:  %7 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %8 = CallInst (:any) %6: function(undefined), empty: any, true: boolean, empty: any, %7: object, %0: classInstance(B), %3: number, %4: undefined|number
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:derived constructor D(x: number): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(D)) %<this>: classInstance(D)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS2: any, %1: environment
// CHECK-NEXT:  %3 = LoadParamInst (:number) %x: number
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS0.C]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:function(undefined)) %4: any, type(function(undefined))
// CHECK-NEXT:  %6 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %7 = CallBuiltinInst (:any) [HermesBuiltin.copyRestArgs]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, 0: number
// CHECK-NEXT:  %8 = CallBuiltinInst (:any) [HermesBuiltin.applyWithNewTarget]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %5: function(undefined), %7: any, %0: classInstance(D), %6: object
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

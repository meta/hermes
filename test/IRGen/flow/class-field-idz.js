/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=C,maker,method -O0 -Werror -typed -dump-ir %s | %FileCheckOrRegen %s --match-full-lines

class A {}
class B {}

class C {
  p1: number;
  p2: number | A;
  p3: void | A;
  p4: A;
  p5: A | B;
  // Symbol is a primitive but has no representable literal default, so it is
  // also IDZ-checked.
  p6: symbol;

  constructor() {
    this.p1 = 1;
    this.p2 = 2;
    this.p3 = undefined;
  }

  maker(): C {
    return new C();
  }

  method() {
    // These should not have ThrowIf
    print(this.p1)
    print(this.p2)
    print(this.p3)
    // These should have ThrowIf
    print(this.p4)
    print(this.p5)
    print(this.p6)
  }
}

return C;

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, A: any, B: any, C: any, ?A.prototype: classInstance(), ?B.prototype: classInstance(), ?C.prototype: classInstance()]

// CHECK:scope %VS1 []

// CHECK:base constructor C(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C)) %<this>: classInstance(C)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS1: any, %1: environment
// CHECK-NEXT:       PrStoreInst 1: number, %0: classInstance(C), 0: number, "p1": string, true: boolean
// CHECK-NEXT:       PrStoreInst 2: number, %0: classInstance(C), 1: number, "p2": string, false: boolean
// CHECK-NEXT:       PrStoreInst undefined: undefined, %0: classInstance(C), 2: number, "p3": string, false: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function maker(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadFrameInst (:any) %0: environment, [%VS0.C]: any
// CHECK-NEXT:  %3 = CheckedTypeCastInst (:function(undefined)) %2: any, type(function(undefined))
// CHECK-NEXT:  %4 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?C.prototype]: classInstance()
// CHECK-NEXT:  %5 = AllocTypedObjectInst (:object) %4: classInstance(), "p1": string, 0: number, "p2": string, 0: number, "p3": string, undefined: undefined, "p4": string, uninit: uninit, "p5": string, uninit: uninit, "p6": string, uninit: uninit
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:classInstance(C)) %5: object
// CHECK-NEXT:  %7 = CallInst (:any) %3: function(undefined), %C(): functionCode, true: boolean, empty: any, %3: function(undefined), %6: classInstance(C)
// CHECK-NEXT:       ReturnInst %6: classInstance(C)
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function method(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C)) %<this>: classInstance(C)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS3: any, %1: environment
// CHECK-NEXT:  %3 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %4 = PrLoadInst (:number) %0: classInstance(C), 0: number, "p1": string
// CHECK-NEXT:  %5 = CallInst (:any) %3: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %4: number
// CHECK-NEXT:  %6 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %7 = PrLoadInst (:number|classInstance(A)) %0: classInstance(C), 1: number, "p2": string
// CHECK-NEXT:  %8 = CallInst (:any) %6: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %7: number|classInstance(A)
// CHECK-NEXT:  %9 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %10 = PrLoadInst (:undefined|classInstance(A)) %0: classInstance(C), 2: number, "p3": string
// CHECK-NEXT:  %11 = CallInst (:any) %9: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %10: undefined|classInstance(A)
// CHECK-NEXT:  %12 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %13 = PrLoadInst (:uninit|classInstance(A)) %0: classInstance(C), 3: number, "p4": string
// CHECK-NEXT:  %14 = ThrowIfInst (:classInstance(A)) %13: uninit|classInstance(A), type(uninit)
// CHECK-NEXT:  %15 = CallInst (:any) %12: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %14: classInstance(A)
// CHECK-NEXT:  %16 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %17 = PrLoadInst (:uninit|classInstance(A)|classInstance(B)) %0: classInstance(C), 4: number, "p5": string
// CHECK-NEXT:  %18 = ThrowIfInst (:classInstance(A)|classInstance(B)) %17: uninit|classInstance(A)|classInstance(B), type(uninit)
// CHECK-NEXT:  %19 = CallInst (:any) %16: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %18: classInstance(A)|classInstance(B)
// CHECK-NEXT:  %20 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %21 = PrLoadInst (:uninit|symbol) %0: classInstance(C), 5: number, "p6": string
// CHECK-NEXT:  %22 = ThrowIfInst (:symbol) %21: uninit|symbol, type(uninit)
// CHECK-NEXT:  %23 = CallInst (:any) %20: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %22: symbol
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

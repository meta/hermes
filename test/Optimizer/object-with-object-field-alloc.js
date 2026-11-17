/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -typed -dump-lir -O %s | %FileCheckOrRegen --match-full-lines %s

class O {
    i: number;
    constructor() {
        this.i = 7;
    }
}

class Foo {
    o0: O;
    o1: O;
    o2: O;
    constructor() {
        this.o0 = new O();
        this.o1 = new O();
        this.o2 = new O();
    }
}

new Foo();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [?O.prototype: classInstance()]

// CHECK:function global(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRLoadConstInst (:null) null: null
// CHECK-NEXT:  %1 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %2 = UnionNarrowTrustedInst (:classInstance()) %1: object
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) empty: any, empty: any, %Array(): functionCode
// CHECK-NEXT:       StorePropertyStrictInst %2: classInstance(), %3: object, "prototype": string
// CHECK-NEXT:  %5 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:classInstance()) %5: object
// CHECK-NEXT:  %7 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 1#"(): functionCode
// CHECK-NEXT:       StorePropertyStrictInst %6: classInstance(), %7: object, "prototype": string
// CHECK-NEXT:  %9 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:  %11 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 2#"(): functionCode
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %11: object, "prototype": string
// CHECK-NEXT:  %13 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %14 = UnionNarrowTrustedInst (:classInstance()) %13: object
// CHECK-NEXT:  %15 = CreateFunctionInst (:function(undefined)) empty: any, empty: any, %Symbol(): functionCode
// CHECK-NEXT:        StorePropertyStrictInst %14: classInstance(), %15: function(undefined), "prototype": string
// CHECK-NEXT:  %17 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance()) %17: object
// CHECK-NEXT:  %19 = CreateFunctionInst (:object) empty: any, empty: any, %Object(): functionCode
// CHECK-NEXT:        StorePropertyStrictInst %18: classInstance(), %19: object, "prototype": string
// CHECK-NEXT:  %21 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %22 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %23 = UnionNarrowTrustedInst (:classInstance()) %22: object
// CHECK-NEXT:        StoreFrameInst %21: environment, %23: classInstance(), [%VS0.?O.prototype]: classInstance()
// CHECK-NEXT:  %25 = CreateFunctionInst (:function(undefined)) empty: any, empty: any, %O(): functionCode
// CHECK-NEXT:        StorePropertyStrictInst %23: classInstance(), %25: function(undefined), "prototype": string
// CHECK-NEXT:  %27 = LIRAllocTypedNonEnumObjectFromBufferInst (:object) %0: null
// CHECK-NEXT:  %28 = UnionNarrowTrustedInst (:classInstance()) %27: object
// CHECK-NEXT:  %29 = CreateFunctionInst (:function(undefined)) %21: environment, %VS0: any, %Foo(): functionCode
// CHECK-NEXT:        StorePropertyStrictInst %28: classInstance(), %29: function(undefined), "prototype": string
// CHECK-NEXT:  %31 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:        ReturnInst %31: undefined
// CHECK-NEXT:function_end

// CHECK:function Array(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:       ReturnInst %0: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 1#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:       ReturnInst %0: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 2#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:       ReturnInst %0: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor Symbol(): any [typed,noReturn]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRGetGlobalObjectInst (:object)
// CHECK-NEXT:  %1 = TryLoadGlobalPropertyInst (:any) %0: object, "TypeError": string
// CHECK-NEXT:  %2 = CreateThisInst (:undefined|object) %1: any, %1: any, empty: any
// CHECK-NEXT:  %3 = LIRLoadConstInst (:string) "Symbol is not a constructor": string
// CHECK-NEXT:  %4 = CallInst (:any) %1: any, empty: any, false: boolean, empty: any, %1: any, %2: undefined|object, %3: string
// CHECK-NEXT:  %5 = GetConstructedObjectInst (:object) %2: undefined|object, %4: any
// CHECK-NEXT:       ThrowInst %5: object
// CHECK-NEXT:function_end

// CHECK:function Object(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:       ReturnInst %0: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor O(): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LIRLoadConstInst (:number) 7: number
// CHECK-NEXT:  %1 = LoadParamInst (:classInstance(O)) %<this>: classInstance(O)
// CHECK-NEXT:       PrStoreInst %0: number, %1: classInstance(O), 0: number, "i": string, true: boolean
// CHECK-NEXT:  %3 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:       ReturnInst %3: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor Foo(): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Foo)) %<this>: classInstance(Foo)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %1: environment, [%VS0.?O.prototype]: classInstance()
// CHECK-NEXT:  %3 = LIRAllocTypedObjectFromBufferInst (:object) %2: classInstance(), "i": string, 0: number
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:classInstance(O)) %3: object
// CHECK-NEXT:  %5 = LIRLoadConstInst (:number) 7: number
// CHECK-NEXT:       PrStoreInst %5: number, %4: classInstance(O), 0: number, "i": string, true: boolean
// CHECK-NEXT:       PrStoreInst %4: classInstance(O), %0: classInstance(Foo), 0: number, "o0": string, false: boolean
// CHECK-NEXT:  %8 = LIRAllocTypedObjectFromBufferInst (:object) %2: classInstance(), "i": string, 0: number
// CHECK-NEXT:  %9 = UnionNarrowTrustedInst (:classInstance(O)) %8: object
// CHECK-NEXT:        PrStoreInst %5: number, %9: classInstance(O), 0: number, "i": string, true: boolean
// CHECK-NEXT:        PrStoreInst %9: classInstance(O), %0: classInstance(Foo), 1: number, "o1": string, false: boolean
// CHECK-NEXT:  %12 = LIRAllocTypedObjectFromBufferInst (:object) %2: classInstance(), "i": string, 0: number
// CHECK-NEXT:  %13 = UnionNarrowTrustedInst (:classInstance(O)) %12: object
// CHECK-NEXT:        PrStoreInst %5: number, %13: classInstance(O), 0: number, "i": string, true: boolean
// CHECK-NEXT:        PrStoreInst %13: classInstance(O), %0: classInstance(Foo), 2: number, "o2": string, false: boolean
// CHECK-NEXT:  %16 = LIRLoadConstInst (:undefined) undefined: undefined
// CHECK-NEXT:        ReturnInst %16: undefined
// CHECK-NEXT:function_end

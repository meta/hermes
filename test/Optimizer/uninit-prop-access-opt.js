/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -fno-inline -typed -dump-ir -O %s | %FileCheckOrRegen --match-full-lines %s

class O {
    i: number;
    constructor() {
        this.i = 7;
    }
}

class Foo {
    o: O;
    y: number;
    constructor() {
        this.y = this.o.i;
        this.o = new O();
    }
}

function f(): void {
    var f: Foo = new Foo();
    var i: number = f.o.i;
    // The question this test is raising is whether the second occurrence of
    // f.o.i below, dominated by the first above, would have a ThrowIfInst for
    // the f.o access.  As of this writing, it does.  It would be nice to be
    // able to optimize that away.  But it requires reasoning about heap field
    // values.  In this case, simple reasoning, since there are no intervening
    // calls that might change the heap; it could even be CSE's.
    i += f.o.i;
}

print(f());

// Auto-generated content below. Please do not modify manually.

// CHECK:function global(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateFunctionInst (:object) empty: any, empty: any, %""(): functionCode
// CHECK-NEXT:  %1 = CallInst [njsf] (:undefined) %0: object, %""(): functionCode, true: boolean, empty: any, undefined: undefined, 0: number
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function ""(): undefined [allCallsitesKnownInStrictMode]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateFunctionInst (:object) empty: any, empty: any, %Array(): functionCode
// CHECK-NEXT:  %1 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %2 = UnionNarrowTrustedInst (:classInstance()) %1: object
// CHECK-NEXT:       StorePropertyStrictInst %2: classInstance(), %0: object, "prototype": string
// CHECK-NEXT:  %4 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 1#"(): functionCode
// CHECK-NEXT:  %5 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:classInstance()) %5: object
// CHECK-NEXT:       StorePropertyStrictInst %6: classInstance(), %4: object, "prototype": string
// CHECK-NEXT:  %8 = CreateFunctionInst (:object) empty: any, empty: any, %"Array 2#"(): functionCode
// CHECK-NEXT:  %9 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %8: object, "prototype": string
// CHECK-NEXT:  %12 = CreateFunctionInst (:object) empty: any, empty: any, %Symbol(): functionCode
// CHECK-NEXT:  %13 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %14 = UnionNarrowTrustedInst (:classInstance()) %13: object
// CHECK-NEXT:        StorePropertyStrictInst %14: classInstance(), %12: object, "prototype": string
// CHECK-NEXT:  %16 = CreateFunctionInst (:object) empty: any, empty: any, %Object(): functionCode
// CHECK-NEXT:  %17 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance()) %17: object
// CHECK-NEXT:        StorePropertyStrictInst %18: classInstance(), %16: object, "prototype": string
// CHECK-NEXT:  %20 = CreateFunctionInst (:object) empty: any, empty: any, %" 1#"(): functionCode
// CHECK-NEXT:  %21 = CallInst [njsf] (:undefined) %20: object, %" 1#"(): functionCode, true: boolean, empty: any, undefined: undefined, 0: number, 0: number
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function Array(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 1#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function "Array 2#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor Symbol(): any [typed,noReturn]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = TryLoadGlobalPropertyInst (:any) globalObject: object, "TypeError": string
// CHECK-NEXT:  %1 = CreateThisInst (:undefined|object) %0: any, %0: any, empty: any
// CHECK-NEXT:  %2 = CallInst (:any) %0: any, empty: any, false: boolean, empty: any, %0: any, %1: undefined|object, "Symbol is not a constructor": string
// CHECK-NEXT:  %3 = GetConstructedObjectInst (:object) %1: undefined|object, %2: any
// CHECK-NEXT:       ThrowInst %3: object
// CHECK-NEXT:function_end

// CHECK:function Object(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS0 [O: object, Foo: undefined|object, ?O.prototype: classInstance(), ?Foo.prototype: classInstance()]

// CHECK:function " 1#"(exports: number): undefined [allCallsitesKnownInStrictMode]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:       StoreFrameInst %0: environment, undefined: undefined, [%VS0.Foo]: undefined|object
// CHECK-NEXT:  %2 = CreateFunctionInst (:object) %0: environment, %VS0: any, %f(): functionCode
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) empty: any, empty: any, %O(): functionCode
// CHECK-NEXT:       StoreFrameInst %0: environment, %3: object, [%VS0.O]: object
// CHECK-NEXT:  %5 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %6 = UnionNarrowTrustedInst (:classInstance()) %5: object
// CHECK-NEXT:       StoreFrameInst %0: environment, %6: classInstance(), [%VS0.?O.prototype]: classInstance()
// CHECK-NEXT:       StorePropertyStrictInst %6: classInstance(), %3: object, "prototype": string
// CHECK-NEXT:  %9 = CreateFunctionInst (:object) %0: environment, %VS0: any, %Foo(): functionCode
// CHECK-NEXT:        StoreFrameInst %0: environment, %9: object, [%VS0.Foo]: undefined|object
// CHECK-NEXT:  %11 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %12 = UnionNarrowTrustedInst (:classInstance()) %11: object
// CHECK-NEXT:        StoreFrameInst %0: environment, %12: classInstance(), [%VS0.?Foo.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %12: classInstance(), %9: object, "prototype": string
// CHECK-NEXT:  %15 = TryLoadGlobalPropertyInst (:any) globalObject: object, "print": string
// CHECK-NEXT:  %16 = CallInst [njsf] (:undefined) %2: object, %f(): functionCode, true: boolean, %0: environment, undefined: undefined, 0: number
// CHECK-NEXT:  %17 = CallInst (:any) %15: any, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, undefined: undefined
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:function f(): undefined [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadFrameInst (:undefined|object) %0: environment, [%VS0.Foo]: undefined|object
// CHECK-NEXT:  %2 = CheckedTypeCastInst (:object) %1: undefined|object, type(object)
// CHECK-NEXT:  %3 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Foo.prototype]: classInstance()
// CHECK-NEXT:  %4 = AllocTypedObjectInst (:object) %3: classInstance(), "o": string, uninit: uninit, "y": string, 0: number
// CHECK-NEXT:  %5 = UnionNarrowTrustedInst (:classInstance(Foo)) %4: object
// CHECK-NEXT:  %6 = CallInst (:undefined) %2: object, %Foo(): functionCode, true: boolean, %0: environment, %2: object, %5: classInstance(Foo)
// CHECK-NEXT:  %7 = PrLoadInst (:uninit|classInstance(O)) %5: classInstance(Foo), 0: number, "o": string
// CHECK-NEXT:  %8 = ThrowIfInst (:classInstance(O)) %7: uninit|classInstance(O), type(uninit)
// CHECK-NEXT:  %9 = PrLoadInst (:uninit|classInstance(O)) %5: classInstance(Foo), 0: number, "o": string
// CHECK-NEXT:  %10 = ThrowIfInst (:classInstance(O)) %9: uninit|classInstance(O), type(uninit)
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor O(): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(O)) %<this>: classInstance(O)
// CHECK-NEXT:       PrStoreInst 7: number, %0: classInstance(O), 0: number, "i": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:base constructor Foo(): undefined [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(Foo)) %<this>: classInstance(Foo)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = PrLoadInst (:uninit|classInstance(O)) %0: classInstance(Foo), 0: number, "o": string
// CHECK-NEXT:  %3 = ThrowIfInst (:classInstance(O)) %2: uninit|classInstance(O), type(uninit)
// CHECK-NEXT:  %4 = PrLoadInst (:number) %3: classInstance(O), 0: number, "i": string
// CHECK-NEXT:       PrStoreInst %4: number, %0: classInstance(Foo), 1: number, "y": string, true: boolean
// CHECK-NEXT:  %6 = LoadFrameInst (:object) %1: environment, [%VS0.O]: object
// CHECK-NEXT:  %7 = LoadFrameInst (:classInstance()) %1: environment, [%VS0.?O.prototype]: classInstance()
// CHECK-NEXT:  %8 = AllocTypedObjectInst (:object) %7: classInstance(), "i": string, 0: number
// CHECK-NEXT:  %9 = UnionNarrowTrustedInst (:classInstance(O)) %8: object
// CHECK-NEXT:  %10 = CallInst (:undefined) %6: object, %O(): functionCode, true: boolean, empty: any, %6: object, %9: classInstance(O)
// CHECK-NEXT:        PrStoreInst %9: classInstance(O), %0: classInstance(Foo), 0: number, "o": string, false: boolean
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

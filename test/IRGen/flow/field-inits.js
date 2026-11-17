/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -typed -dump-ir %s -O0 | %FileCheckOrRegen --match-full-lines %s

class A {
    y = 7; // Field init for y.
    z: number;
    constructor() {
        // y is initialized before first statement of ctor without super.
        this.z = this.y + 1;
    }
}

// Fields are initialized in order.
class B {
    x = 76;
    y = this.x + 1;
    constructor() {}
}

// Implicit constructor is called, and does field inits.
class C0 {
    x = 775;
    m(): number {
        return this.x + 1;
    }
}

// In a class with a superclass, a method invocation via super
// works and gets the right value; the super() constructor is
// executed before the field inits of the subclass.
class C1 extends C0 {
    y = super.m() + 1;
    constructor() {
        super();
    }
}

// Field initializers may reference variables in their lexical scope.
function f(i: number): number {
    class A {
        x = i * 1000; // Gets the value of i from the invocation.
        constructor() {}
    }
    var a = new A();
    return a.x;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:       StoreStackInst undefined: undefined, %1: any
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) %0: environment, %VS0: any, %""(): functionCode
// CHECK-NEXT:  %4 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:  %5 = UnionNarrowTrustedInst (:exactObject()) %4: object
// CHECK-NEXT:  %6 = CallInst [njsf] (:any) %3: object, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %5: exactObject()
// CHECK-NEXT:       StoreStackInst %6: any, %1: any
// CHECK-NEXT:  %8 = LoadStackInst (:any) %1: any
// CHECK-NEXT:       ReturnInst %8: any
// CHECK-NEXT:function_end

// CHECK:scope %VS1 [exports: any, A: any, B: any, C0: any, C1: any, f: any, <fieldInitFuncVar:A>: object, ?A.prototype: classInstance(), <fieldInitFuncVar:B>: object, ?B.prototype: classInstance(), <fieldInitFuncVar:C0>: object, ?C0.prototype: classInstance(), <fieldInitFuncVar:C1>: object, ?C1.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.A]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.B]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C0]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C1]: any
// CHECK-NEXT:  %8 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %f(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %8: function(number), [%VS1.f]: any
// CHECK-NEXT:  %10 = CreateFunctionInst (:object) %1: environment, %VS1: any, %<instance_members_initializer:A>(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: object, [%VS1.<fieldInitFuncVar:A>]: object
// CHECK-NEXT:  %12 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %A(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %12: function(undefined), [%VS1.A]: any
// CHECK-NEXT:  %14 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %15 = UnionNarrowTrustedInst (:classInstance()) %14: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %15: classInstance(), [%VS1.?A.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %15: classInstance(), %12: function(undefined), "prototype": string
// CHECK-NEXT:  %18 = CreateFunctionInst (:object) %1: environment, %VS1: any, %<instance_members_initializer:B>(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %18: object, [%VS1.<fieldInitFuncVar:B>]: object
// CHECK-NEXT:  %20 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %B(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %20: function(undefined), [%VS1.B]: any
// CHECK-NEXT:  %22 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %23 = UnionNarrowTrustedInst (:classInstance()) %22: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %23: classInstance(), [%VS1.?B.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %23: classInstance(), %20: function(undefined), "prototype": string
// CHECK-NEXT:  %26 = CreateFunctionInst (:object) %1: environment, %VS1: any, %<instance_members_initializer:C0>(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %26: object, [%VS1.<fieldInitFuncVar:C0>]: object
// CHECK-NEXT:  %28 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C0(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %28: object, [%VS1.C0]: any
// CHECK-NEXT:  %30 = CreateFunctionInst (:function(number)) %1: environment, %VS1: any, %m(): functionCode
// CHECK-NEXT:  %31 = AllocTypedNonEnumObjectInst (:object) null: null, "m": string, %30: function(number)
// CHECK-NEXT:  %32 = UnionNarrowTrustedInst (:classInstance()) %31: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %32: classInstance(), [%VS1.?C0.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %32: classInstance(), %28: object, "prototype": string
// CHECK-NEXT:  %35 = LoadFrameInst (:any) %1: environment, [%VS1.C0]: any
// CHECK-NEXT:  %36 = CheckedTypeCastInst (:object) %35: any, type(object)
// CHECK-NEXT:  %37 = CreateFunctionInst (:object) %1: environment, %VS1: any, %<instance_members_initializer:C1>(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %37: object, [%VS1.<fieldInitFuncVar:C1>]: object
// CHECK-NEXT:  %39 = CreateFunctionInst (:function(undefined)) %1: environment, %VS1: any, %C1(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %39: function(undefined), [%VS1.C1]: any
// CHECK-NEXT:  %41 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C0.prototype]: classInstance()
// CHECK-NEXT:  %42 = UnionNarrowTrustedInst (:object) %41: classInstance()
// CHECK-NEXT:  %43 = PrLoadInst (:function(number)) %42: object, 0: number, "m": string
// CHECK-NEXT:  %44 = AllocTypedNonEnumObjectInst (:object) %42: object, "m": string, %43: function(number)
// CHECK-NEXT:  %45 = UnionNarrowTrustedInst (:classInstance()) %44: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %45: classInstance(), [%VS1.?C1.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %45: classInstance(), %39: function(undefined), "prototype": string
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [i: any, A: any, a: any, <fieldInitFuncVar:A>: object, ?A.prototype: classInstance()]

// CHECK:function f(i: number): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:number) %i: number
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: number, [%VS2.i]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.A]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS2.a]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:object) %1: environment, %VS2: any, %"<instance_members_initializer:A> 1#"(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: object, [%VS2.<fieldInitFuncVar:A>]: object
// CHECK-NEXT:  %8 = CreateFunctionInst (:function(undefined)) %1: environment, %VS2: any, %"A 1#"(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %8: function(undefined), [%VS2.A]: any
// CHECK-NEXT:  %10 = AllocTypedNonEnumObjectInst (:object) null: null
// CHECK-NEXT:  %11 = UnionNarrowTrustedInst (:classInstance()) %10: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %11: classInstance(), [%VS2.?A.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %11: classInstance(), %8: function(undefined), "prototype": string
// CHECK-NEXT:  %14 = LoadFrameInst (:any) %1: environment, [%VS2.A]: any
// CHECK-NEXT:  %15 = CheckedTypeCastInst (:function(undefined)) %14: any, type(function(undefined))
// CHECK-NEXT:  %16 = LoadFrameInst (:classInstance()) %1: environment, [%VS2.?A.prototype]: classInstance()
// CHECK-NEXT:  %17 = AllocTypedObjectInst (:object) %16: classInstance(), "x": string, undefined: undefined
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:classInstance(A)) %17: object
// CHECK-NEXT:  %19 = CallInst (:any) %15: function(undefined), %"A 1#"(): functionCode, true: boolean, empty: any, %15: function(undefined), %18: classInstance(A)
// CHECK-NEXT:        StoreFrameInst %1: environment, %18: classInstance(A), [%VS2.a]: any
// CHECK-NEXT:  %21 = LoadFrameInst (:any) %1: environment, [%VS2.a]: any
// CHECK-NEXT:  %22 = CheckedTypeCastInst (:classInstance(A)) %21: any, type(classInstance(A))
// CHECK-NEXT:  %23 = PrLoadInst (:any) %22: classInstance(A), 0: number, "x": string
// CHECK-NEXT:  %24 = CheckedTypeCastInst (:number) %23: any, type(number)
// CHECK-NEXT:        ReturnInst %24: number
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function <instance_members_initializer:A>(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(A)) %<this>: classInstance(A)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS3: any, %1: environment
// CHECK-NEXT:       PrStoreInst 7: number, %0: classInstance(A), 0: number, "y": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS4 []

// CHECK:base constructor A(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(A)) %<this>: classInstance(A)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS4: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:object) %1: environment, [%VS1.<fieldInitFuncVar:A>]: object
// CHECK-NEXT:  %4 = CallInst (:undefined) %3: object, %<instance_members_initializer:A>(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(A)
// CHECK-NEXT:  %5 = PrLoadInst (:number) %0: classInstance(A), 0: number, "y": string
// CHECK-NEXT:  %6 = BinaryAddInst (:any) %5: number, 1: number
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:number) %6: any, type(number)
// CHECK-NEXT:       PrStoreInst %7: number, %0: classInstance(A), 1: number, "z": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 []

// CHECK:function <instance_members_initializer:B>(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(B)) %<this>: classInstance(B)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS5: any, %1: environment
// CHECK-NEXT:       PrStoreInst 76: number, %0: classInstance(B), 0: number, "x": string, true: boolean
// CHECK-NEXT:  %4 = PrLoadInst (:number) %0: classInstance(B), 0: number, "x": string
// CHECK-NEXT:  %5 = BinaryAddInst (:any) %4: number, 1: number
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:number) %5: any, type(number)
// CHECK-NEXT:       PrStoreInst %6: number, %0: classInstance(B), 1: number, "y": string, false: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS6 []

// CHECK:base constructor B(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(B)) %<this>: classInstance(B)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS6: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:object) %1: environment, [%VS1.<fieldInitFuncVar:B>]: object
// CHECK-NEXT:  %4 = CallInst (:undefined) %3: object, %<instance_members_initializer:B>(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(B)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS7 []

// CHECK:function <instance_members_initializer:C0>(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C0)) %<this>: classInstance(C0)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS7: any, %1: environment
// CHECK-NEXT:       PrStoreInst 775: number, %0: classInstance(C0), 0: number, "x": string, true: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS8 []

// CHECK:function C0(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C0)) %<this>: classInstance(C0)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS8: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:object) %1: environment, [%VS1.<fieldInitFuncVar:C0>]: object
// CHECK-NEXT:  %4 = CallInst (:undefined) %3: object, %<instance_members_initializer:C0>(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(C0)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS9 []

// CHECK:function m(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C0)) %<this>: classInstance(C0)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS9: any, %1: environment
// CHECK-NEXT:  %3 = PrLoadInst (:number) %0: classInstance(C0), 0: number, "x": string
// CHECK-NEXT:  %4 = BinaryAddInst (:any) %3: number, 1: number
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:number) %4: any, type(number)
// CHECK-NEXT:       ReturnInst %5: number
// CHECK-NEXT:function_end

// CHECK:scope %VS10 []

// CHECK:function <instance_members_initializer:C1>(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C1)) %<this>: classInstance(C1)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS10: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C0.prototype]: classInstance()
// CHECK-NEXT:  %4 = PrLoadInst (:function(number)) %3: classInstance(), 0: number, "m": string
// CHECK-NEXT:  %5 = CallInst [njsf] (:number) %4: function(number), empty: any, false: boolean, empty: any, undefined: undefined, %0: classInstance(C1)
// CHECK-NEXT:  %6 = BinaryAddInst (:any) %5: number, 1: number
// CHECK-NEXT:  %7 = CheckedTypeCastInst (:number) %6: any, type(number)
// CHECK-NEXT:       PrStoreInst %7: number, %0: classInstance(C1), 1: number, "y": string, false: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS11 []

// CHECK:derived constructor C1(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(C1)) %<this>: classInstance(C1)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS11: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:any) %1: environment, [%VS1.C0]: any
// CHECK-NEXT:  %4 = CheckedTypeCastInst (:object) %3: any, type(object)
// CHECK-NEXT:  %5 = GetNewTargetInst (:object) %new.target: object
// CHECK-NEXT:  %6 = CallInst (:any) %4: object, empty: any, false: boolean, empty: any, %5: object, %0: classInstance(C1)
// CHECK-NEXT:  %7 = LoadFrameInst (:object) %1: environment, [%VS1.<fieldInitFuncVar:C1>]: object
// CHECK-NEXT:  %8 = CallInst (:undefined) %7: object, %<instance_members_initializer:C1>(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(C1)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS12 []

// CHECK:function "<instance_members_initializer:A> 1#"(): undefined
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(A)) %<this>: classInstance(A)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS2: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS12: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:any) %1: environment, [%VS2.i]: any
// CHECK-NEXT:  %4 = CheckedTypeCastInst (:number) %3: any, type(number)
// CHECK-NEXT:  %5 = BinaryMultiplyInst (:any) %4: number, 1000: number
// CHECK-NEXT:  %6 = CheckedTypeCastInst (:number) %5: any, type(number)
// CHECK-NEXT:       PrStoreInst %6: number, %0: classInstance(A), 0: number, "x": string, false: boolean
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS13 []

// CHECK:base constructor "A 1#"(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:classInstance(A)) %<this>: classInstance(A)
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS2: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS13: any, %1: environment
// CHECK-NEXT:  %3 = LoadFrameInst (:object) %1: environment, [%VS2.<fieldInitFuncVar:A>]: object
// CHECK-NEXT:  %4 = CallInst (:undefined) %3: object, %"<instance_members_initializer:A> 1#"(): functionCode, true: boolean, empty: any, undefined: undefined, %0: classInstance(A)
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

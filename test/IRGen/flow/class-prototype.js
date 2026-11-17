/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -O0 -typed -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

'use strict';

class C {
  method(): number {
    return 1;
  }
}

class D extends C {
  own(): number {
    return 2;
  }
}

// `.prototype` is a named property load on the constructor, and the method is
// then read directly from the resulting home object: no TypedLoadParentInst,
// because the home object is not the parent of itself. The IDZ check comes
// from the field type; home object slots are in fact always initialized.
C.prototype.method.call(new C());
D.prototype.method.call(new D());
D.prototype.own.call(new D());

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 []

// CHECK:function global(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = CreateScopeInst (:environment) %VS0: any, empty: any
// CHECK-NEXT:  %1 = AllocStackInst (:any) $?anon_0_ret: any
// CHECK-NEXT:       StoreStackInst undefined: undefined, %1: any
// CHECK-NEXT:  %3 = CreateFunctionInst (:object) %0: environment, %VS0: any, %""(): functionCode
// CHECK-NEXT:  %4 = AllocObjectLiteralInst (:object) empty: any
// CHECK-NEXT:  %5 = CallInst [njsf] (:any) %3: object, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %4: object
// CHECK-NEXT:       StoreStackInst %5: any, %1: any
// CHECK-NEXT:  %7 = LoadStackInst (:any) %1: any
// CHECK-NEXT:       ReturnInst %7: any
// CHECK-NEXT:function_end

// CHECK:scope %VS1 [exports: any, C: any, D: any, ?C.prototype: classInstance(), ?D.prototype: classInstance()]

// CHECK:function ""(exports: any): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:any) %exports: any
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: any, [%VS1.exports]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.C]: any
// CHECK-NEXT:       StoreFrameInst %1: environment, undefined: undefined, [%VS1.D]: any
// CHECK-NEXT:  %6 = CreateFunctionInst (:object) %1: environment, %VS1: any, %C(): functionCode
// CHECK-NEXT:       StoreFrameInst %1: environment, %6: object, [%VS1.C]: any
// CHECK-NEXT:  %8 = CreateFunctionInst (:object) %1: environment, %VS1: any, %method(): functionCode
// CHECK-NEXT:  %9 = AllocTypedNonEnumObjectInst (:object) null: null, "method": string, %8: object
// CHECK-NEXT:  %10 = UnionNarrowTrustedInst (:classInstance()) %9: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %10: classInstance(), [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %10: classInstance(), %6: object, "prototype": string
// CHECK-NEXT:  %13 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %14 = CheckedTypeCastInst (:object) %13: any, type(object)
// CHECK-NEXT:  %15 = CreateFunctionInst (:object) %1: environment, %VS1: any, %D(): functionCode
// CHECK-NEXT:        StoreFrameInst %1: environment, %15: object, [%VS1.D]: any
// CHECK-NEXT:  %17 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %18 = UnionNarrowTrustedInst (:object) %17: classInstance()
// CHECK-NEXT:  %19 = PrLoadInst (:object) %18: object, 0: number, "method": string
// CHECK-NEXT:  %20 = CreateFunctionInst (:object) %1: environment, %VS1: any, %own(): functionCode
// CHECK-NEXT:  %21 = AllocTypedNonEnumObjectInst (:object) %18: object, "method": string, %19: object, "own": string, %20: object
// CHECK-NEXT:  %22 = UnionNarrowTrustedInst (:classInstance()) %21: object
// CHECK-NEXT:        StoreFrameInst %1: environment, %22: classInstance(), [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:        StorePropertyStrictInst %22: classInstance(), %15: object, "prototype": string
// CHECK-NEXT:  %25 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %26 = CheckedTypeCastInst (:object) %25: any, type(object)
// CHECK-NEXT:  %27 = LoadPropertyInst (:classInstance()) %26: object, "prototype": string
// CHECK-NEXT:  %28 = PrLoadInst (:uninit|object) %27: classInstance(), 0: number, "method": string
// CHECK-NEXT:  %29 = ThrowIfInst (:object) %28: uninit|object, type(uninit)
// CHECK-NEXT:  %30 = LoadFrameInst (:any) %1: environment, [%VS1.C]: any
// CHECK-NEXT:  %31 = CheckedTypeCastInst (:object) %30: any, type(object)
// CHECK-NEXT:  %32 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?C.prototype]: classInstance()
// CHECK-NEXT:  %33 = AllocTypedObjectInst (:object) %32: classInstance()
// CHECK-NEXT:  %34 = UnionNarrowTrustedInst (:classInstance(C)) %33: object
// CHECK-NEXT:  %35 = CallInst (:any) %29: object, empty: any, false: boolean, empty: any, undefined: undefined, %34: classInstance(C)
// CHECK-NEXT:  %36 = CheckedTypeCastInst (:number) %35: any, type(number)
// CHECK-NEXT:  %37 = LoadFrameInst (:any) %1: environment, [%VS1.D]: any
// CHECK-NEXT:  %38 = CheckedTypeCastInst (:object) %37: any, type(object)
// CHECK-NEXT:  %39 = LoadPropertyInst (:classInstance()) %38: object, "prototype": string
// CHECK-NEXT:  %40 = PrLoadInst (:uninit|object) %39: classInstance(), 0: number, "method": string
// CHECK-NEXT:  %41 = ThrowIfInst (:object) %40: uninit|object, type(uninit)
// CHECK-NEXT:  %42 = LoadFrameInst (:any) %1: environment, [%VS1.D]: any
// CHECK-NEXT:  %43 = CheckedTypeCastInst (:object) %42: any, type(object)
// CHECK-NEXT:  %44 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:  %45 = AllocTypedObjectInst (:object) %44: classInstance()
// CHECK-NEXT:  %46 = UnionNarrowTrustedInst (:classInstance(D)) %45: object
// CHECK-NEXT:  %47 = CallInst (:any) %43: object, %D(): functionCode, true: boolean, empty: any, %43: object, %46: classInstance(D)
// CHECK-NEXT:  %48 = CallInst (:any) %41: object, empty: any, false: boolean, empty: any, undefined: undefined, %46: classInstance(D)
// CHECK-NEXT:  %49 = CheckedTypeCastInst (:number) %48: any, type(number)
// CHECK-NEXT:  %50 = LoadFrameInst (:any) %1: environment, [%VS1.D]: any
// CHECK-NEXT:  %51 = CheckedTypeCastInst (:object) %50: any, type(object)
// CHECK-NEXT:  %52 = LoadPropertyInst (:classInstance()) %51: object, "prototype": string
// CHECK-NEXT:  %53 = PrLoadInst (:uninit|object) %52: classInstance(), 1: number, "own": string
// CHECK-NEXT:  %54 = ThrowIfInst (:object) %53: uninit|object, type(uninit)
// CHECK-NEXT:  %55 = LoadFrameInst (:any) %1: environment, [%VS1.D]: any
// CHECK-NEXT:  %56 = CheckedTypeCastInst (:object) %55: any, type(object)
// CHECK-NEXT:  %57 = LoadFrameInst (:classInstance()) %1: environment, [%VS1.?D.prototype]: classInstance()
// CHECK-NEXT:  %58 = AllocTypedObjectInst (:object) %57: classInstance()
// CHECK-NEXT:  %59 = UnionNarrowTrustedInst (:classInstance(D)) %58: object
// CHECK-NEXT:  %60 = CallInst (:any) %56: object, %D(): functionCode, true: boolean, empty: any, %56: object, %59: classInstance(D)
// CHECK-NEXT:  %61 = CallInst (:any) %54: object, empty: any, false: boolean, empty: any, undefined: undefined, %59: classInstance(D)
// CHECK-NEXT:  %62 = CheckedTypeCastInst (:number) %61: any, type(number)
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS2 []

// CHECK:function C(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS3 []

// CHECK:function method(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:       ReturnInst 1: number
// CHECK-NEXT:function_end

// CHECK:scope %VS4 []

// CHECK:function D(): any
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS4: any, %0: environment
// CHECK-NEXT:       ReturnInst undefined: undefined
// CHECK-NEXT:function_end

// CHECK:scope %VS5 []

// CHECK:function own(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS1: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS5: any, %0: environment
// CHECK-NEXT:       ReturnInst 2: number
// CHECK-NEXT:function_end

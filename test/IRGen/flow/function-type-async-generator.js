/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -fno-std-globals -Werror -O0 -typed -Xdump-functions=main -dump-ir %s | %FileCheckOrRegen --match-full-lines %s

// The closure of a typed async function, async arrow or generator is created
// from the outer function, so it carries the lowered function type, like the
// closure of a plain typed function, and needs no cast when it is stored into
// a typed binding.

'use strict';

function main(): void {
  async function af(): number {
    return 1;
  }
  function* gf(): number {
    yield 1;
    return 2;
  }
  const aa = async (): number => 1;
  function pf(): number {
    return 1;
  }
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, main: any]

// CHECK:scope %VS1 [?anon_0_this: any, ?anon_1_new.target: undefined|object, af: any, gf: any, aa: any, pf: any]

// CHECK:function main(): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = LoadParamInst (:any) %<this>: any
// CHECK-NEXT:  %1 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %2 = CreateScopeInst (:environment) %VS1: any, %1: environment
// CHECK-NEXT:       StoreFrameInst %2: environment, %0: any, [%VS1.?anon_0_this]: any
// CHECK-NEXT:  %4 = GetNewTargetInst (:undefined|object) %new.target: undefined|object
// CHECK-NEXT:       StoreFrameInst %2: environment, %4: undefined|object, [%VS1.?anon_1_new.target]: undefined|object
// CHECK-NEXT:       StoreFrameInst %2: environment, undefined: undefined, [%VS1.aa]: any
// CHECK-NEXT:  %7 = CreateFunctionInst (:function(number)) %2: environment, %VS1: any, %af(): functionCode
// CHECK-NEXT:       StoreFrameInst %2: environment, %7: function(number), [%VS1.af]: any
// CHECK-NEXT:  %9 = CreateFunctionInst (:function(number)) %2: environment, %VS1: any, %gf(): functionCode
// CHECK-NEXT:        StoreFrameInst %2: environment, %9: function(number), [%VS1.gf]: any
// CHECK-NEXT:  %11 = CreateFunctionInst (:function(number)) %2: environment, %VS1: any, %pf(): functionCode
// CHECK-NEXT:        StoreFrameInst %2: environment, %11: function(number), [%VS1.pf]: any
// CHECK-NEXT:  %13 = CreateFunctionInst (:function(number)) %2: environment, %VS1: any, %aa(): functionCode
// CHECK-NEXT:        StoreFrameInst %2: environment, %13: function(number), [%VS1.aa]: any
// CHECK-NEXT:        ReturnInst undefined: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -dump-ir -Xdump-functions=bar -Xcustom-opt=frameloadstoreopts,functionanalysis %s | %FileCheckOrRegen %s --match-full-lines

// Ensure that FunctionAnalysis needs to follow checked casts.
// bar() below uses a checked cast before calling foo().
// The IR output we are checking shows the cast and that CallInst instruction has the
// correct target.

var foo: any = function () {}

function bar() {
    return (foo as () => void)();
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, foo: any, bar: any]

// CHECK:scope %VS1 []

// CHECK:function bar(): any [allCallsitesKnownInStrictMode,unreachable]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadFrameInst (:any) %0: environment, [%VS0.foo]: any
// CHECK-NEXT:  %3 = CheckedTypeCastInst (:function(undefined)) %2: any, type(function(undefined))
// CHECK-NEXT:  %4 = CallInst [njsf] (:undefined) %3: function(undefined), %foo(): functionCode, true: boolean, %0: environment, undefined: undefined, undefined: undefined
// CHECK-NEXT:       ReturnInst %4: undefined
// CHECK-NEXT:function_end

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=foo -typed -dump-ir -O %s | %FileCheckOrRegen %s --match-full-lines

return function foo(x: string, y: string): string[] {
  return [x + y, x + 'abc', x + y + 'abc' + 'def' + y];
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [?Array.prototype: classInstance()]

// CHECK:function foo(x: string, y: string): array(string) [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:string) %x: string
// CHECK-NEXT:  %2 = LoadParamInst (:string) %y: string
// CHECK-NEXT:  %3 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype]: classInstance()
// CHECK-NEXT:  %4 = AllocFastArrayInst (:object) 3: number, %3: classInstance()
// CHECK-NEXT:  %5 = UnionNarrowTrustedInst (:array(string)) %4: object
// CHECK-NEXT:  %6 = StringConcatInst (:string) %1: string, %2: string
// CHECK-NEXT:       FastArrayPushInst %6: string, %5: array(string)
// CHECK-NEXT:  %8 = StringConcatInst (:string) %1: string, "abc": string
// CHECK-NEXT:       FastArrayPushInst %8: string, %5: array(string)
// CHECK-NEXT:  %10 = StringConcatInst (:string) %1: string, %2: string, "abcdef": string, %2: string
// CHECK-NEXT:        FastArrayPushInst %10: string, %5: array(string)
// CHECK-NEXT:        ReturnInst %5: array(string)
// CHECK-NEXT:function_end

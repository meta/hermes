/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=mk,objKeys -typed -dump-ir -O %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %shermes -Xdump-functions=mk -typed -dump-ir -O0 -verify-ir %s | %FileCheck --check-prefix=CHKO0 %s

// Typed array allocations carry their lowered array(T) type, so returning
// them as T[] needs no runtime cast. The prototype operand keeps the refined
// type of the Array<T> home object.

(function () {

function mk(x: number): number[] {
  'noinline';
  return [x, 2];
}
// CHKO0-LABEL: function mk(
// CHKO0: [[PROTO:%[0-9]+]] = LoadFrameInst (:classInstance()) {{.*}}?Array.prototype
// CHKO0-NEXT: AllocFastArrayInst (:object) 2: number, [[PROTO]]: classInstance()

function objKeys(o: {a: number}): string[] {
  'noinline';
  return $SHBuiltin.objectKeys(o);
}

print(mk(1)[0], mk(3)[0], objKeys({a: 1})[0], objKeys({a: 2}).length);

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [?Array.prototype: classInstance(), ?Array.prototype#1: classInstance()]

// CHECK:function mk(x: number): array(number) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:number) %x: number
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype#1]: classInstance()
// CHECK-NEXT:  %3 = AllocFastArrayInst (:object) 2: number, %2: classInstance()
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:array(number)) %3: object
// CHECK-NEXT:       FastArrayPushInst %1: number, %4: array(number)
// CHECK-NEXT:       FastArrayPushInst 2: number, %4: array(number)
// CHECK-NEXT:       ReturnInst %4: array(number)
// CHECK-NEXT:function_end

// CHECK:function objKeys(o: object): array(string) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:object) %o: object
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype]: classInstance()
// CHECK-NEXT:  %3 = CallBuiltinInst (:object) [HermesBuiltin.objectKeys]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %1: object, %2: classInstance()
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:array(string)) %3: object
// CHECK-NEXT:       ReturnInst %4: array(string)
// CHECK-NEXT:function_end

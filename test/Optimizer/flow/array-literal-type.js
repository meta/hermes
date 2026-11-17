/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Xdump-functions=mk,objKeys,objValues,objEntries -typed -dump-ir -O %s | %FileCheckOrRegen --match-full-lines %s
// RUN: %shermes -Xdump-functions=mk -typed -dump-ir -O0 -verify-ir %s | %FileCheck --check-prefix=CHKO0 %s

// Typed array allocations carry their lowered array(T) type, so returning
// them as T[] needs no runtime cast. That includes the arrays returned by
// $SHBuiltin.objectKeys/objectValues/objectEntries. The prototype operand
// keeps the refined type of the Array<T> home object.

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

function objValues(o: {a: number}): mixed[] {
  'noinline';
  return $SHBuiltin.objectValues(o);
}

function objEntries(o: {a: number}): [string, mixed][] {
  'noinline';
  return $SHBuiltin.objectEntries(o);
}

print(mk(1)[0], mk(3)[0], objKeys({a: 1})[0], objKeys({a: 2}).length);
print(objValues({a: 1})[0], objValues({a: 2}).length);
print(objEntries({a: 1})[0][0], objEntries({a: 2})[0][1]);

})();

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [?Array.prototype: classInstance(), ?Array.prototype#1: classInstance(), ?Array.prototype#2: classInstance(), ?Array.prototype#3: classInstance()]

// CHECK:function mk(x: number): array(number) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:number) %x: number
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype#3]: classInstance()
// CHECK-NEXT:  %3 = AllocFastArrayInst (:object) 2: number, %2: classInstance()
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:array(number)) %3: object
// CHECK-NEXT:       FastArrayPushInst %1: number, %4: array(number)
// CHECK-NEXT:       FastArrayPushInst 2: number, %4: array(number)
// CHECK-NEXT:       ReturnInst %4: array(number)
// CHECK-NEXT:function_end

// CHECK:function objKeys(o: exactObject(a: number)): array(string) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:exactObject(a: number)) %o: exactObject(a: number)
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype]: classInstance()
// CHECK-NEXT:  %3 = CallBuiltinInst (:object) [HermesBuiltin.objectKeys]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %1: exactObject(a: number), %2: classInstance()
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:array(string)) %3: object
// CHECK-NEXT:       ReturnInst %4: array(string)
// CHECK-NEXT:function_end

// CHECK:function objValues(o: exactObject(a: number)): array(any) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:exactObject(a: number)) %o: exactObject(a: number)
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype#1]: classInstance()
// CHECK-NEXT:  %3 = CallBuiltinInst (:object) [HermesBuiltin.objectValues]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %1: exactObject(a: number), %2: classInstance()
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:array(any)) %3: object
// CHECK-NEXT:       ReturnInst %4: array(any)
// CHECK-NEXT:function_end

// CHECK:function objEntries(o: exactObject(a: number)): array(tuple(string, any)) [allCallsitesKnownInStrictMode,typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = LoadParamInst (:exactObject(a: number)) %o: exactObject(a: number)
// CHECK-NEXT:  %2 = LoadFrameInst (:classInstance()) %0: environment, [%VS0.?Array.prototype#2]: classInstance()
// CHECK-NEXT:  %3 = CallBuiltinInst (:object) [HermesBuiltin.objectEntries]: number, empty: any, false: boolean, empty: any, undefined: undefined, undefined: undefined, %1: exactObject(a: number), %2: classInstance()
// CHECK-NEXT:  %4 = UnionNarrowTrustedInst (:array(tuple(string, any))) %3: object
// CHECK-NEXT:       ReturnInst %4: array(tuple(string, any))
// CHECK-NEXT:function_end

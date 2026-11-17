/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -O0 -typed -dump-ir -Xdump-functions=tagOf,valueOf,slotOf %s | %FileCheckOrRegen %s --match-full-lines

// Reading a field common to every arm of an object-literal union. When the
// field is at the same slot in every arm it lowers to a single PrLoadInst at
// that slot; when the slots differ it lowers to a by-name LoadPropertyInst.

// Discriminant read: result is the union of the literal arm types (string).
function tagOf(s: {tag: 'circle', radius: number} | {tag: 'square', side: number}): string {
  return s.tag;
}

// Differing field types at the same slot: the load is typed as the union.
function valueOf(p: {k: number} | {k: string}): number | string {
  return p.k;
}

// Field at a different slot in each arm: lowers to a by-name LoadPropertyInst.
function slotOf(p: {tag: string, x: number} | {x: number, tag: string}): string {
  return p.tag;
}

// Auto-generated content below. Please do not modify manually.

// CHECK:scope %VS0 [exports: any, tagOf: any, valueOf: any, slotOf: any]

// CHECK:scope %VS1 [s: any]

// CHECK:function tagOf(s: exactObject(tag: string, radius: number)|exactObject(tag: string, side: number)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS1: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(tag: string, radius: number)|exactObject(tag: string, side: number)) %s: exactObject(tag: string, radius: number)|exactObject(tag: string, side: number)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(tag: string, radius: number)|exactObject(tag: string, side: number), [%VS1.s]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS1.s]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:exactObject(tag: string, radius: number)|exactObject(tag: string, side: number)) %4: any, type(exactObject(tag: string, radius: number)|exactObject(tag: string, side: number))
// CHECK-NEXT:  %6 = PrLoadInst (:string) %5: exactObject(tag: string, radius: number)|exactObject(tag: string, side: number), 0: number, "tag": string
// CHECK-NEXT:       ReturnInst %6: string
// CHECK-NEXT:function_end

// CHECK:scope %VS2 [p: any]

// CHECK:function valueOf(p: exactObject(k: string)|exactObject(k: number)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS2: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(k: string)|exactObject(k: number)) %p: exactObject(k: string)|exactObject(k: number)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(k: string)|exactObject(k: number), [%VS2.p]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS2.p]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:exactObject(k: string)|exactObject(k: number)) %4: any, type(exactObject(k: string)|exactObject(k: number))
// CHECK-NEXT:  %6 = PrLoadInst (:string|number) %5: exactObject(k: string)|exactObject(k: number), 0: number, "k": string
// CHECK-NEXT:       ReturnInst %6: string|number
// CHECK-NEXT:function_end

// CHECK:scope %VS3 [p: any]

// CHECK:function slotOf(p: exactObject(tag: string, x: number)|exactObject(x: number, tag: string)): any [typed]
// CHECK-NEXT:%BB0:
// CHECK-NEXT:  %0 = GetParentScopeInst (:environment) %VS0: any, %parentScope: environment
// CHECK-NEXT:  %1 = CreateScopeInst (:environment) %VS3: any, %0: environment
// CHECK-NEXT:  %2 = LoadParamInst (:exactObject(tag: string, x: number)|exactObject(x: number, tag: string)) %p: exactObject(tag: string, x: number)|exactObject(x: number, tag: string)
// CHECK-NEXT:       StoreFrameInst %1: environment, %2: exactObject(tag: string, x: number)|exactObject(x: number, tag: string), [%VS3.p]: any
// CHECK-NEXT:  %4 = LoadFrameInst (:any) %1: environment, [%VS3.p]: any
// CHECK-NEXT:  %5 = CheckedTypeCastInst (:exactObject(tag: string, x: number)|exactObject(x: number, tag: string)) %4: any, type(exactObject(tag: string, x: number)|exactObject(x: number, tag: string))
// CHECK-NEXT:  %6 = LoadPropertyInst (:string) %5: exactObject(tag: string, x: number)|exactObject(x: number, tag: string), "tag": string
// CHECK-NEXT:       ReturnInst %6: string
// CHECK-NEXT:function_end

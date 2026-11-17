/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O0 -typed -Xdump-functions=main -dump-ir %s | %FileCheck --implicit-check-not='classInstance(Array' %s

// `new Array<number>()` and `new` of a subclass of Array<number> allocate a
// fast array with the class's home object as its prototype. The allocation is
// narrowed once to the class's lowered type: array(number) for Array<number>
// itself, the type Array<number> lowers to everywhere else, not
// classInstance(Array); classInstance(Foo) for the subclass.

'use strict';

class Foo extends Array<number> {
  m(): number {
    return 1;
  }
}

function main(): void {
  const a: Array<number> = new Array<number>();
  const f: Foo = new Foo();
}

// CHECK-LABEL: function main(
// CHECK: [[APROTO:%[0-9]+]] = LoadFrameInst (:classInstance()) {{%[0-9]+}}: environment, [%VS{{[0-9]+}}.?Array.prototype#{{[0-9]+}}]: classInstance()
// CHECK-NEXT: [[AALLOC:%[0-9]+]] = AllocFastArrayInst (:object) 0: number, [[APROTO]]: classInstance()
// CHECK-NEXT: [[ANARROW:%[0-9]+]] = UnionNarrowTrustedInst (:array(number)) [[AALLOC]]: object
// CHECK-NEXT: StoreFrameInst {{%[0-9]+}}: environment, [[ANARROW]]: array(number), [%VS{{[0-9]+}}.a]: any
// CHECK: [[FPROTO:%[0-9]+]] = LoadFrameInst (:classInstance()) {{%[0-9]+}}: environment, [%VS{{[0-9]+}}.?Foo.prototype]: classInstance()
// CHECK-NEXT: [[FALLOC:%[0-9]+]] = AllocFastArrayInst (:object) 0: number, [[FPROTO]]: classInstance()
// CHECK-NEXT: [[FNARROW:%[0-9]+]] = UnionNarrowTrustedInst (:classInstance(Foo)) [[FALLOC]]: object
// CHECK-NEXT: CallInst (:any) {{%[0-9]+}}: object, %Foo(): functionCode, true: boolean, empty: any, {{%[0-9]+}}: object, [[FNARROW]]: classInstance(Foo)
// CHECK-NEXT: StoreFrameInst {{%[0-9]+}}: environment, [[FNARROW]]: classInstance(Foo), [%VS{{[0-9]+}}.f]: any

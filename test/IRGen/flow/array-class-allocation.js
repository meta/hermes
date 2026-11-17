/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O0 -typed -Xdump-functions=main -dump-ir %s | %FileCheck --implicit-check-not='classInstance(Array' %s

// `new Array<number>()` allocates through emitTypedClassAllocation, which
// lowers the Array ClassType itself rather than a Flow Type wrapper. The
// allocation is narrowed straight to array(number), the type that Array<number>
// lowers to everywhere else, not to classInstance(Array).

'use strict';

function main(): Array<number> {
  return new Array<number>();
}

// CHECK-LABEL: function main(
// CHECK: [[ALLOC:%[0-9]+]] = AllocTypedObjectInst (:object)
// CHECK-NEXT: [[NARROW:%[0-9]+]] = UnionNarrowTrustedInst (:array(number)) [[ALLOC]]: object
// CHECK-NEXT: ReturnInst [[NARROW]]: array(number)

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s

print("splice");
// CHECK: splice

let removed = [];
let arr = new Proxy([], {
	deleteProperty(target, p) {
		removed.push(p);
		return Reflect.deleteProperty(target, p);
	},
});

arr.push('a', 'b', 'c');
arr.splice(0);
print(removed);
// CHECK-NEXT: 2,1,0

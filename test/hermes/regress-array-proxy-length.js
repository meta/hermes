/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s

print("proxy-length");
// CHECK: proxy-length

let output = [];
let arr = new Proxy([], {
	defineProperty(target, property, attributes) {
		output.push("def-" + property);
		return Reflect.defineProperty(target, property, attributes)
	},
	deleteProperty(target, property) {
		output.push("del-" + property);
		return Reflect.deleteProperty(target, property)
	},
});

arr.push('a', 'b', 'c')
arr.pop()
arr.shift()
arr.length = 1
print(output);
// CHECK-NEXT: def-0,def-1,def-2,def-length,del-2,def-length,def-0,del-1,def-length,def-length

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xes6-proxy -gc-sanitize-handles=1 %s | %FileCheck --match-full-lines %s

// Regression: the lookup of a computed key along the prototype chain kept
// the key's SymbolID in a bare local across the flush of the GCScope whose
// handle rooted it. With a key that is not a string, the next object on the
// chain allocates when it converts the key to an array index, and the
// SymbolID that then reaches the proxy at the end of the chain was dead.
// Only a build with the handle sanitizer collects at that point.

var seen = [];
var proxy = new Proxy({}, {
  get(target, key) {
    seen.push(typeof key + ":" + String(key));
    return "from proxy";
  },
  has(target, key) {
    seen.push("has:" + String(key));
    return true;
  },
});

// An object with indexed storage in the middle of the chain.
var arr = [];
Object.setPrototypeOf(arr, proxy);
var obj = Object.create(arr);

print(obj[1.5]);
// CHECK: from proxy
print(obj[-1]);
// CHECK-NEXT: from proxy
print(obj[true]);
// CHECK-NEXT: from proxy
print(1.5 in obj);
// CHECK-NEXT: true
print(seen.join(" "));
// CHECK-NEXT: string:1.5 string:-1 string:true has:1.5

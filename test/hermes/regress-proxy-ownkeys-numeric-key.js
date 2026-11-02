/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xes6-proxy -non-strict -O -target=HBC %s | %FileCheck --match-full-lines %s

// Regression test for https://github.com/facebook/hermes/issues/1609
//
// A Proxy [[OwnPropertyKeys]] over a non-extensible target used to throw
// "ownKeys target is non-extensible but key is missing from trap result"
// whenever the target had an integer-index own key (e.g. "12345"). Hermes
// represents integer-index keys internally as numbers, but the ownKeys trap
// result holds them (per spec) as strings, so the SameValue comparison in the
// non-extensible invariant check never matched.

function ownKeysTrap(t) {
  return Reflect.ownKeys(t);
}

// Case 1: preventExtensions with a configurable integer-index key.
// Exercises the "target configurable keys" invariant loop.
var t1 = {};
Object.defineProperty(t1, '12345', {
  value: 1,
  enumerable: true,
  configurable: true,
  writable: true,
});
Object.preventExtensions(t1);
var p1 = new Proxy(t1, {ownKeys: ownKeysTrap});
print('case1:', Object.keys(p1).join(','));
// CHECK: case1: 12345

// Case 2: frozen (non-configurable) integer-index keys.
// Exercises the "target non-configurable keys" invariant loop.
var t2 = Object.freeze({0: 'a', 42: 'b'});
var p2 = new Proxy(t2, {ownKeys: ownKeysTrap});
print('case2:', Object.getOwnPropertyNames(p2).join(','));
// CHECK-NEXT: case2: 0,42

// Case 3: mixed integer-index and string keys, frozen; spec ordering is
// ascending integer indices first, then string keys in insertion order.
var t3 = Object.freeze({b: 1, 2: 2, a: 3, 1: 4});
var p3 = new Proxy(t3, {ownKeys: ownKeysTrap});
print('case3:', Reflect.ownKeys(p3).join(','));
// CHECK-NEXT: case3: 1,2,b,a

// Case 4: integer-index keys alongside symbols must still work.
var s = Symbol('s');
var t4 = {7: 'x'};
t4[s] = 'y';
Object.preventExtensions(t4);
var p4 = new Proxy(t4, {ownKeys: ownKeysTrap});
print('case4:', Reflect.ownKeys(p4).map(String).join(','));
// CHECK-NEXT: case4: 7,Symbol(s)

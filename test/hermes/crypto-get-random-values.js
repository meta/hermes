/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: core_extensions
// RUN: %hermes -O -target=HBC -Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
"use strict";

print('crypto.getRandomValues');
// CHECK-LABEL: crypto.getRandomValues

print(typeof crypto, typeof crypto.getRandomValues);
// CHECK-NEXT: object function
print(Object.prototype.toString.call(crypto));
// CHECK-NEXT: [object Crypto]

// Returns its argument.
var a = new Uint8Array(16);
print(crypto.getRandomValues(a) === a);
// CHECK-NEXT: true

// All integer TypedArray types are accepted.
for (const T of [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array,
                 Uint16Array, Int32Array, Uint32Array, BigInt64Array,
                 BigUint64Array]) {
  const arr = crypto.getRandomValues(new T(8));
  print(arr.constructor.name, arr.length);
}
// CHECK-NEXT: Int8Array 8
// CHECK-NEXT: Uint8Array 8
// CHECK-NEXT: Uint8ClampedArray 8
// CHECK-NEXT: Int16Array 8
// CHECK-NEXT: Uint16Array 8
// CHECK-NEXT: Int32Array 8
// CHECK-NEXT: Uint32Array 8
// CHECK-NEXT: BigInt64Array 8
// CHECK-NEXT: BigUint64Array 8

// A zero-length view is allowed.
var empty = new Uint8Array(0);
print(crypto.getRandomValues(empty) === empty);
// CHECK-NEXT: true

// The maximum quota of 65536 bytes is allowed, and the odds of 64 KiB of
// random data using only a single byte value are astronomically small.
var big = new Uint8Array(65536);
crypto.getRandomValues(big);
print(new Set(big).size > 1);
// CHECK-NEXT: true

// One byte over the quota is rejected.
try {
  crypto.getRandomValues(new Uint8Array(65537));
} catch (e) {
  print(e.name, e.message);
  // CHECK-NEXT: RangeError crypto.getRandomValues: byte length exceeds the quota (65536)
}

// The quota applies to bytes, not elements.
try {
  crypto.getRandomValues(new Uint32Array(16385));
} catch (e) {
  print(e.name);
  // CHECK-NEXT: RangeError
}

// Only the subarray's window is written.
var backing = new Uint8Array(64);
var view = backing.subarray(16, 48);
crypto.getRandomValues(view);
var outsideUntouched = true;
for (var i = 0; i < 16; i++) {
  outsideUntouched = outsideUntouched && backing[i] === 0;
}
for (var i = 48; i < 64; i++) {
  outsideUntouched = outsideUntouched && backing[i] === 0;
}
print(outsideUntouched);
// CHECK-NEXT: true

// Non-integer-TypedArray arguments are rejected.
for (const bad of [undefined, null, 42, 'str', [1, 2, 3], {length: 4},
                   new Float32Array(4), new Float64Array(4),
                   new DataView(new ArrayBuffer(8)), new ArrayBuffer(8)]) {
  try {
    crypto.getRandomValues(bad);
    print('no error');
  } catch (e) {
    print(e.name, e.message);
  }
}
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray
// CHECK-NEXT: TypeError crypto.getRandomValues: argument must be an integer TypedArray

// A view on a detached ArrayBuffer is rejected.
var ab = new ArrayBuffer(8);
var u8 = new Uint8Array(ab);
HermesInternal.detachArrayBuffer(ab);
try {
  crypto.getRandomValues(u8);
} catch (e) {
  print(e.name, e.message);
  // CHECK-NEXT: TypeError crypto.getRandomValues called on a detached ArrayBuffer
}

// getRandomValues keeps working even after user code tampers with globals
// it depends on.
globalThis.TypeError = function FakeTypeError() {};
globalThis.RangeError = function FakeRangeError() {};
globalThis.Uint8Array = function FakeUint8Array() {};
var tampered = new Int8Array(8);
print(crypto.getRandomValues(tampered) === tampered);
// CHECK-NEXT: true

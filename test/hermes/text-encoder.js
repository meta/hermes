/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: core_extensions
// RUN: LC_ALL=en_US.UTF-8 %hermes -O -target=HBC -Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
"use strict";

print('TextEncoder');
// CHECK-LABEL: TextEncoder

var encoder = new TextEncoder();
print(Object.prototype.toString.call(encoder));
// CHECK-NEXT: [object TextEncoder]

const desc = Object.getOwnPropertyDescriptor(TextEncoder.prototype, 'encoding');
print(desc.enumerable);
// CHECK-NEXT: true
print(desc.configurable);
// CHECK-NEXT: true

print(encoder.encoding);
// CHECK-NEXT: utf-8

try {
  const b = {};
  TextEncoder.prototype.encode.call(b, '');
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextEncoder.prototype.encode() called on non-TextEncoder object
}

try {
  TextEncoder.prototype.encode.call(undefined, '');
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextEncoder.prototype.encode() called on non-TextEncoder object
}

let result = encoder.encode('test');
print(Object.prototype.toString.call(result));
// CHECK-NEXT: [object Uint8Array]

print(result.length, result.join(' '));
// CHECK-NEXT: 4 116 101 115 116

result = encoder.encode('');
print(result.length);
// CHECK-NEXT: 0

// Test default argument behavior (issue #1857)
result = encoder.encode();
print(result.length);
// CHECK-NEXT: 0

result = encoder.encode(undefined);
print(result.length);
// CHECK-NEXT: 0

// Explicit null should stringify to "null" (4 bytes)
result = encoder.encode(null);
print(result.length);
// CHECK-NEXT: 4

// String "undefined" should encode (9 bytes)
result = encoder.encode("undefined");
print(result.length);
// CHECK-NEXT: 9

result = encoder.encode('\x0D');
print(result.length, result[0]);
// CHECK-NEXT: 1 13

// Test UTF-16 conversion
result = encoder.encode('\u{2191}\u{2192}');
print(Object.prototype.toString.call(result));
// CHECK-NEXT: [object Uint8Array]

print(result.length, result.join(' '));
// CHECK-NEXT: 6 226 134 145 226 134 146

// A surrogate pair
result = encoder.encode('\u{D83D}\u{DE03}');
print(result.length, result.join(' '));
// CHECK-NEXT: 4 240 159 152 131

// Incomplete surrogate
result = encoder.encode('\u{D83D}');
print(result.length, result.join(' '));
// CHECK-NEXT: 3 239 191 189

result = new Uint8Array(4);

try {
  const b = {};
  TextEncoder.prototype.encodeInto.call(b, '', result);
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextEncoder.prototype.encodeInto() called on non-TextEncoder object
}

try {
  TextEncoder.prototype.encodeInto.call(undefined, '', result);
} catch (e) {
  print(e.message);
  // CHECK-NEXT: TextEncoder.prototype.encodeInto() called on non-TextEncoder object
}

// Test the ASCII case that just fits within the provided buffer
let stats = encoder.encodeInto('test', result);
print(stats.read, stats.written);
// CHECK-NEXT: 4 4
print(result[0], result[1], result[2], result[3]);
// CHECK-NEXT: 116 101 115 116

stats = encoder.encodeInto('', result);
print(stats.read, stats.written);
// CHECK-NEXT: 0 0

// Unlike encode(), encodeInto() has required parameters per the spec.
// Passing undefined for the source stringifies to "undefined" (9 bytes).
result = new Uint8Array(20);
stats = encoder.encodeInto(undefined, result);
print(stats.read, stats.written);
// CHECK-NEXT: 9 9
print(result.slice(0, 9).join(' '));
// CHECK-NEXT: 117 110 100 101 102 105 110 101 100

// Restore 4-byte buffer for remaining tests
result = new Uint8Array(4);

// ASCII case that does NOT fit within the provided buffer
stats = encoder.encodeInto('testing', result);
print(stats.read, stats.written);
// CHECK-NEXT: 4 4
print(result[0], result[1], result[2], result[3]);
// CHECK-NEXT: 116 101 115 116

// ASCII case that is smaller than the provided buffer
stats = encoder.encodeInto('abc', result);
print(stats.read, stats.written);
// CHECK-NEXT: 3 3
print(result[0], result[1], result[2]);
// CHECK-NEXT: 97 98 99

// UTF-16 case that fits within the provided buffer
stats = encoder.encodeInto('\u{2191}', result);
print(stats.read, stats.written);
// CHECK-NEXT: 1 3
print(result[0], result[1], result[2]);
// CHECK-NEXT: 226 134 145

// UTF-16 case that does NOT fit within the provided buffer
stats = encoder.encodeInto('\u{2191}\u{2192}', result);
print(stats.read, stats.written);
// CHECK-NEXT: 1 3
print(result[0], result[1], result[2]);
// CHECK-NEXT: 226 134 145

// Surrogate case that just fits within the provided buffer
stats = encoder.encodeInto('\u{D83D}\u{DE03}', result);
print(stats.read, stats.written);
// CHECK-NEXT: 2 4
print(result[0], result[1], result[2], result[3]);
// CHECK-NEXT: 240 159 152 131

// Test encodeInto with detached ArrayBuffer throws TypeError
var ab = new ArrayBuffer(10);
var u8 = new Uint8Array(ab);
HermesInternal.detachArrayBuffer(ab);
try {
  encoder.encodeInto('test', u8);
} catch (e) {
  print(e.name, e.message);
  // CHECK-NEXT: TypeError TextEncoder.prototype.encodeInto called on a detached ArrayBuffer
}

// WebIDL reports a bad receiver, a bad destination and a missing argument as a
// TypeError, like the detached case above.
function nameOfThrow(f) {
  try {
    f();
    return 'no throw';
  } catch (e) {
    return e.name;
  }
}
print(nameOfThrow(function () { TextEncoder.prototype.encode.call({}, 'a'); }));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () {
  TextEncoder.prototype.encodeInto.call({}, 'a', new Uint8Array(4));
}));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { encoder.encodeInto('a', {}); }));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { encoder.encodeInto('a', 123); }));
// CHECK-NEXT: TypeError
print(nameOfThrow(function () { encoder.encodeInto('a'); }));
// CHECK-NEXT: TypeError

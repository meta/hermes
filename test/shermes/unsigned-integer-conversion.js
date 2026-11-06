/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -exec -O %s | %FileCheck %s
// RUN: %shermes -exec -O0 %s | %FileCheck %s

function unsigned(value, shift) {
  return value >>> shift;
}

var cases = [
  [-1, 4294967295],
  [-2147483648, 2147483648],
  [-4294967297, 4294967295],
  [-1.5, 4294967295],
  [0, 0],
  [4294967295, 4294967295],
  [4294967296, 0],
  [9007199254740991, 4294967295],
  [9223372036854775808, 0],
  [-9223372036854775808, 0],
  [18446744073709551616, 0],
  [1e20, 1661992960],
  [-1e20, 2632974336],
  [Infinity, 0],
  [-Infinity, 0],
  [NaN, 0],
];
for (var i = 0; i < cases.length; ++i) {
  // Calls through an array keep the inputs dynamic, exercising the runtime
  // conversion rather than only the compiler's constant folder.
  if (unsigned(cases[i][0], 0) !== cases[i][1] ||
      unsigned(cases[i][0], 16) !== Math.floor(cases[i][1] / 65536)) {
    throw new Error('Unsigned conversion failed at case ' + i);
  }
}
print('unsigned conversions passed');
// CHECK: unsigned conversions passed

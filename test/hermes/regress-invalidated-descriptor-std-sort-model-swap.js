/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s

// The comparator replaces the elements with getter-only accessors.
function makeCmp(a) {
  return function (x, y) {
    a.__defineGetter__(1, function () {
      delete a[0];
      return 1;
    });
    a.__defineGetter__(0, function () {
      return 1;
    });
    return -1;
  };
}

// Both arrays are sorted as a copy, so writing the result back to a
// getter-only element throws, whether the array takes the fast path or not.
function check(arr) {
  try {
    arr.sort(makeCmp(arr));
    print("no throw");
  } catch (e) {
    print(e.name);
  }
}
check([0, 1]);
// CHECK: TypeError
class Sub extends Array {}
check(Sub.of(0, 1));
// CHECK-NEXT: TypeError

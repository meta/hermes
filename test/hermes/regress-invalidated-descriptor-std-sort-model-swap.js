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

// A plain array is sorted as a copy, and writing the result back to a
// getter-only element throws.
var a = [0, 1];
try {
  a.sort(makeCmp(a));
  print("no throw");
} catch (e) {
  print(e.name);
}
// CHECK: TypeError

// A subclass instance is sorted in place by StandardSortModel, whose swap
// must not use a descriptor the getter invalidated.
class Sub extends Array {}
var b = Sub.of(0, 1);
b.sort(makeCmp(b));
print("done");
// CHECK-NEXT: done

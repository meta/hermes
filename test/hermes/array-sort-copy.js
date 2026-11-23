/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s

// Array.prototype.sort reads the elements into a list, sorts the list and
// writes it back (ES2025 23.1.3.30). Sorting the array in place instead
// fails the first three checks.

print("sort-copy");
// CHECK-LABEL: sort-copy

// A comparator that throws leaves the array unchanged.
var t = [3, 1, 2, 5, 4, 9, 8, 7, 6, 0];
var n = 0;
try {
  t.sort(function (a, b) {
    if (++n === 5) throw new Error("boom");
    return a - b;
  });
} catch (e) {
  print(e.message + " " + t);
}
// CHECK-NEXT: boom 3,1,2,5,4,9,8,7,6,0

// The comparator only sees the original elements, and the write-back
// overwrites changes it makes to the array.
var s = [5, 4, 3, 2, 1, 0, 9, 8, 7, 6];
var seen = [];
s.sort(function (a, b) {
  seen.push(a, b);
  s.length = 2;
  s[0] = "x";
  return a - b;
});
print(s + " " + seen.indexOf("x"));
// CHECK-NEXT: 0,1,2,3,4,5,6,7,8,9 -1

// The write-back calls a setter the comparator defined.
var acc = [3, 2, 1, 0];
var setValues = [];
acc.sort(function (a, b) {
  if (!Object.getOwnPropertyDescriptor(acc, "1").get) {
    Object.defineProperty(acc, "1", {
      get: function () { return "G"; },
      set: function (v) { setValues.push(v); },
      configurable: true,
    });
  }
  return a - b;
});
print(acc + " " + setValues);
// CHECK-NEXT: 0,G,2,3 1

// Elements the comparator appends past the original length are kept.
var p = [5, 4, 3, 2, 1, 0, 9, 8, 7, 6];
p.sort(function (a, b) { p.push(100); return a - b; });
print(p.slice(0, 11) + " " + (p.length > 10));
// CHECK-NEXT: 0,1,2,3,4,5,6,7,8,9,100 true

// Writing back to an array the comparator froze throws.
var f = [3, 2, 1];
try {
  f.sort(function (a, b) { Object.freeze(f); return a - b; });
} catch (e) {
  print(e.name + " " + f);
}
// CHECK-NEXT: TypeError 3,2,1

// Default sort compares ToString of each value; undefined sorts last.
print(String([10, 9, undefined, 1, -0, 25].sort()));
// CHECK-NEXT: 0,1,10,25,9,

// Holes are moved to the end and stay holes.
var h = [3, , 1, , 2, undefined];
h.sort();
print(h.length + " " + (3 in h) + " " + (4 in h) + " " + h[3]);
// CHECK-NEXT: 6 true false undefined

// When the comparator turns off the fast path, the write-back deletes the
// indices past the sorted elements, including an accessor.
var hg = [3, , 1, , 2];
hg.sort(function (a, b) {
  Object.defineProperty(hg, "4", {
    get: function () { return 9; },
    configurable: true,
  });
  return a - b;
});
print(hg + " " + hg.length + " " + (3 in hg) + " " + (4 in hg));
// CHECK-NEXT: 1,2,3,, 5 false false

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s

// Array.prototype.sort and toSorted without a comparator compare ToString
// of the elements, which may be cached only for primitives.

print("sort-default");
// CHECK-LABEL: sort-default

// Primitives of each type; undefined sorts last.
var prims = [10, "b", true, null, undefined, 2n, -0, NaN, "a", 1, -Infinity];
print(JSON.stringify(prims.toSorted().map(String)));
// CHECK-NEXT: ["-Infinity","0","1","10","2","NaN","a","b","null","true","undefined"]
print(String(prims.sort()) === String(prims.toSorted()));
// CHECK-NEXT: true

// Objects among primitives compare by their ToString.
var five = {toString: function () { return "5"; }};
print(String([five, 10, 1, "3", 7n].sort()));
// CHECK-NEXT: 1,10,3,5,7

// Objects are converted on every comparison: each of the at least 19
// comparisons of 20 objects converts both of them.
var calls = 0;
var objs = [];
for (var i = 0; i < 20; i++) {
  objs.push({v: (i * 7) % 20, toString: function () { calls++; return String(this.v); }});
}
objs.sort();
print(objs.map(function (o) { return o.v; }).join(",") + " " + (calls >= 38));
// CHECK-NEXT: 0,1,10,11,12,13,14,15,16,17,18,19,2,3,4,5,6,7,8,9 true

// A symbol throws only when it is compared, and leaves the array unchanged.
print([Symbol("s")].sort().length);
// CHECK-NEXT: 1
var withSym = [3, Symbol("s"), 1];
try {
  withSym.sort();
} catch (e) {
  print(e.name + " " + withSym.length + " " + withSym[0] + " " + withSym[2]);
}
// CHECK-NEXT: TypeError 3 3 1

// Holes stay at the end.
var holey = [10, , 9, , 1];
holey.sort();
print(holey.length + " " + holey[0] + "," + holey[1] + "," + holey[2] + " " + (3 in holey) + " " + (4 in holey));
// CHECK-NEXT: 5 1,10,9 false false

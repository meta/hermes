/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// Regression: JSON.parse preallocated property storage for every key/value
// pair. A duplicate key left the object with fewer properties than storage
// slots, and a later cached property add appended past the slot assigned by
// the HiddenClass, writing outside the property storage.

function addExtra(o) {
  o.extra = 'extra';
}

function mk(n, dup) {
  var s = '{';
  for (var i = 0; i < n; i++) s += (i ? ',' : '') + '"k' + i + '":' + i;
  if (dup) s += ',"k0":"dup"';
  return s + '}';
}

var plain = mk(8, false);
addExtra(JSON.parse(plain));
addExtra(JSON.parse(plain));

var o = JSON.parse(mk(8, true));
addExtra(o);
print(o.extra, o.k0, o.k7, Object.keys(o).length);
// CHECK: extra dup 7 9
print(Object.keys(o).join());
// CHECK-NEXT: k0,k1,k2,k3,k4,k5,k6,k7,extra

var keep = [];
var wrong = 0;
for (var i = 0; i < 1000; i++) {
  var p = JSON.parse(mk(8, true));
  addExtra(p);
  if (p.extra !== 'extra') wrong++;
  keep.push(p, {x: i});
}
var bad = 0;
for (var j = 0; j < keep.length; j += 2) {
  if (keep[j + 1].x !== j / 2) bad++;
}
print(wrong, bad);
// CHECK-NEXT: 0 0

var nested = JSON.parse('{"a":' + mk(8, true) + ',"a":' + mk(8, true) + '}');
addExtra(nested.a);
print(nested.a.extra, Object.keys(nested).length);
// CHECK-NEXT: extra 1

var big = JSON.parse(mk(70, true));
addExtra(big);
print(big.extra, big.k0, Object.keys(big).length);
// CHECK-NEXT: extra dup 71

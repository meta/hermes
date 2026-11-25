/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -gc-sanitize-handles=0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -emit-binary -out %t.hbc %s && %hermes -gc-sanitize-handles=0 %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s -Wx,-gc-sanitize-handles=0 | %FileCheck --match-full-lines %s

// Array.prototype.slice of a large array that passes arrayFastPathCheck().
// The storage of a result this large is allocated separately from the array,
// in the old generation, so copying young objects into it needs write
// barriers. This took minutes with HandleSan, so -gc-sanitize-handles=0 is
// passed to reduce the chances of tests timing out.

var N = 1100000;
// Fill with a string, since HandleSan builds allocate a box for every number
// that is stored.
var large = new Array(N).fill('s');
for (var i = 0; i < N; i += 1000) large[i] = i + 0.5;
// Make the last elements young objects.
for (var i = N - 1000; i < N; i++) large[i] = {v: i};
var lc = large.slice(1);
gc();
var ok = lc.length === N - 1;
for (var i = 999; ok && i < N - 1001; i += 1000) {
  ok = lc[i] === i + 1.5 && lc[i + 1] === 's';
}
for (var i = N - 1001; ok && i < N - 1; i++) {
  ok = lc[i] === large[i + 1] && lc[i].v === i + 1;
}
print(ok, lc.length, lc[0], lc[999], lc[N - 2].v);
// CHECK: true 1099999 s 1000.5 1099999

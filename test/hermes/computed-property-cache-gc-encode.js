/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 %s | %FileCheck --match-full-lines %s

// A computed put served by the computed property cache, of a value whose
// encode allocates: a double that does not fit inline, which HV32 boxes,
// and under the handle sanitizer, every number. The put probes the cache
// before it encodes the value; a collection during the encode can then
// empty the entry it found, so the put must probe again after the encode.
// With the handle sanitizer the collection moves every class, which empties
// every entry. Keys come from Object.keys(), so that they are uniqued.
//
// What the test exercises depends on the build:
// - HV32 or BOXED with the handle sanitizer: every number put takes the
//   boxing path, and the re-probe misses and falls through to the slow
//   path.
// - HV32 or BOXED without it: the 0.1 puts take the boxing path and are
//   usually served through the entry found by the re-probe.
// - HV64: nothing boxes; the test checks the values only.

'use strict';

function main() {
  var keys = Object.keys({a: 0, b: 0, c: 0, d: 0});
  var kc = keys[2];
  var kd = keys[3];

  // A store to an existing property: fill a Slot entry for c, slot 2, with
  // integers, then store doubles through it.
  var o = {a: 1, b: 2, c: 3};
  for (var i = 0; i < 10; ++i) o[kc] = i;
  var bad = 0;
  for (var i = 0; i < 10; ++i) {
    o[kc] = i * 0.1;
    if (o.a !== 1 || o.b !== 2 || o.c !== i * 0.1) ++bad;
  }
  print('slot', o.a, o.b, o.c, bad);
// CHECK: slot 1 2 0.9 0

  // An add: fill an Add entry for d on fresh objects of one shape with
  // integers, then add doubles through it. The objects are made first, so
  // that nothing allocates (and empties the entry) between one add and the
  // probe of the next.
  var objs = [];
  for (var i = 0; i < 20; ++i) objs.push({a: 1, b: 2, c: 3});
  for (var i = 0; i < 10; ++i) objs[i][kd] = i;
  bad = 0;
  var p;
  for (var i = 10; i < 20; ++i) {
    p = objs[i];
    p[kd] = i * 0.1;
    if (p.a !== 1 || p.b !== 2 || p.c !== 3 || p.d !== i * 0.1) ++bad;
  }
  print('add', p.a, p.b, p.c, p.d, bad);
// CHECK-NEXT: add 1 2 3 1.9000000000000001 0
}

main();

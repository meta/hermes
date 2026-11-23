/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xmicrotask-queue -O -gc-sanitize-handles=0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xmicrotask-queue -O -gc-sanitize-handles=0 -gc-alloc-young=false -gc-revert-to-yg-at-tti=true %s | %FileCheck --match-full-lines %s
// REQUIRES: gc_hades

// Non-compacting YG collections only visit the weak slots and WeakMap entries
// that were set to a YG object since the previous YG collection. Check that
// the weak references to YG objects created in all the possible ways are
// updated when their referents are evacuated, and cleared when their referents
// die. In Debug builds, HadesGC::verifyRecordedWeakSlots also checks at the
// start of every YG collection that all the weak references to YG objects are
// recorded.
// Run the checks with N references of each kind. With N = 10, each list gets
// fewer records than ManagedChunkedList::kMaxRecords between YG collections,
// so the YG collections only visit the recorded slots. With N = 100, the
// records are dropped, and the YG collections visit every slot.

"use strict";

print("weak young gen");
// CHECK-LABEL: weak young gen

function numYGCollections() {
  var stats = HermesInternal.getInstrumentedStats();
  return stats.js_gcSpecific.js_numYGCollections;
}

globalThis.sink = null;
function allocateUntilYoungCollection() {
  var start = numYGCollections();
  while (numYGCollections() === start) {
    globalThis.sink = new Array(64);
  }
}

// Run the checks with N references of each kind, and then call next() in a
// later job.
function test(N, next) {
  print("N =", N);

  // Objects, WeakMaps and entries that the first YG collection (or YG
  // promotion, with -gc-alloc-young=false) moves to the OG.
  var oldKeys = [];
  var oldValues = [];
  var youngMapValues = [];
  for (var i = 0; i < N; ++i) {
    oldKeys.push({k: i});
    oldValues.push({v: i});
    youngMapValues.push({v: i});
  }
  var oldMap = new WeakMap();
  for (var i = 0; i < N; ++i) {
    oldMap.set(oldKeys[i], oldValues[i]);
  }
  var oldMap2 = new WeakMap();
  var oldSet = new WeakSet();
  var registry = new FinalizationRegistry(function () {});
  allocateUntilYoungCollection();
  // Stop YG promotions, so that the next YG collections evacuate the YG.
  HermesInternal.ttiReached();

  // WeakRefs to YG objects that survive.
  var targets = [];
  var refs = [];
  for (var i = 0; i < N; ++i) {
    targets.push({v: i});
    refs.push(new WeakRef(targets[i]));
  }
  // YG keys in an OG map, with OG values.
  var youngKeys = [];
  for (var i = 0; i < N; ++i) {
    youngKeys.push({k: i});
    oldMap2.set(youngKeys[i], oldValues[i]);
  }
  // YG keys in an OG set.
  for (var i = 0; i < N; ++i) {
    oldSet.add(youngKeys[i]);
  }
  // YG values replacing the values of OG entries.
  for (var i = 0; i < N; ++i) {
    oldMap.set(oldKeys[i], {v: 2 * i});
  }
  // A YG map, with OG keys and values.
  var youngMap = new WeakMap();
  for (var i = 0; i < N; ++i) {
    youngMap.set(oldKeys[i], youngMapValues[i]);
  }
  // YG targets and unregister tokens in an OG registry.
  var tokens = [];
  for (var i = 0; i < N; ++i) {
    tokens.push({t: i});
    registry.register(targets[i], i, tokens[i]);
  }
  allocateUntilYoungCollection();

  var sum = 0;
  for (var i = 0; i < N; ++i) {
    sum += refs[i].deref().v;
  }
  print("weakref", sum);

  sum = 0;
  for (var i = 0; i < N; ++i) {
    sum += oldMap2.get(youngKeys[i]).v;
  }
  print("young keys", sum);

  sum = 0;
  for (var i = 0; i < N; ++i) {
    if (oldSet.has(youngKeys[i])) {
      ++sum;
    }
  }
  print("weakset young keys", sum);

  sum = 0;
  for (var i = 0; i < N; ++i) {
    sum += oldMap.get(oldKeys[i]).v;
  }
  print("young values", sum);

  sum = 0;
  for (var i = 0; i < N; ++i) {
    sum += youngMap.get(oldKeys[i]).v;
  }
  print("young map", sum);

  sum = 0;
  for (var i = 0; i < N; ++i) {
    if (registry.unregister(tokens[i])) {
      ++sum;
    }
  }
  print("unregister", sum);

  // Drop the YG map above (now in the OG), and its values, which nothing else
  // references. A full collection must then free the values. It would keep
  // them alive if the entries still referred to the location of the map in the
  // YG (the map was the only YG object they referred to), since OG collections
  // treat all YG objects as alive. The targets of WeakRefs are kept alive until
  // the end of the job that creates them, so collect in a later job.
  var youngMapValueRefs = [];
  for (var i = 0; i < N; ++i) {
    youngMapValueRefs.push(new WeakRef(youngMapValues[i]));
  }
  youngMap = null;
  youngMapValues = null;
  setTimeout(function () {
    gc();
    var alive = 0;
    for (var i = 0; i < N; ++i) {
      if (youngMapValueRefs[i].deref() !== undefined) {
        ++alive;
      }
    }
    print("dead young map values", alive);

    // WeakRefs to YG objects that die. The targets are kept alive until the
    // end of this job, so check them in a later job.
    var deadRefs = [];
    var start = numYGCollections();
    for (var i = 0; i < N; ++i) {
      deadRefs.push(new WeakRef({v: i}));
    }
    // The targets are not expected to die if a YG collection moved them to the
    // OG while they were kept alive.
    var promoted = numYGCollections() !== start;
    setTimeout(function () {
      allocateUntilYoungCollection();
      var alive = 0;
      for (var i = 0; i < N; ++i) {
        if (deadRefs[i].deref() !== undefined) {
          ++alive;
        }
      }
      print("dead weakref", promoted ? 0 : alive);
      next();
    }, 0);
  }, 0);
}

test(10, function () {
  test(100, function () {});
});

// CHECK-NEXT: N = 10
// CHECK-NEXT: weakref 45
// CHECK-NEXT: young keys 45
// CHECK-NEXT: weakset young keys 10
// CHECK-NEXT: young values 90
// CHECK-NEXT: young map 45
// CHECK-NEXT: unregister 10
// CHECK-NEXT: dead young map values 0
// CHECK-NEXT: dead weakref 0

// CHECK-NEXT: N = 100
// CHECK-NEXT: weakref 4950
// CHECK-NEXT: young keys 4950
// CHECK-NEXT: weakset young keys 100
// CHECK-NEXT: young values 9900
// CHECK-NEXT: young map 4950
// CHECK-NEXT: unregister 100
// CHECK-NEXT: dead young map values 0
// CHECK-NEXT: dead weakref 0

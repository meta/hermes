/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xes6-promise %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xmicrotask-queue %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -emit-binary -out %t.hbc %s && %hermes -Xes6-promise %t.hbc | %FileCheck --match-full-lines %s

print('promise');
// CHECK-LABEL: promise

print(HermesInternal.hasPromise())
// CHECK-NEXT: true

print('all' in Promise);
// CHECK-NEXT: true

print('allSettled' in Promise);
// CHECK-NEXT: true

print('any' in Promise);
// CHECK-NEXT: true

print('withResolvers' in Promise);
// CHECK-NEXT: true


var promise = new Promise(function(res, rej) {
  res('success!');
});

promise.then(function(message) {
  print('Resolved:', message);
});
// CHECK-NEXT: Resolved: success!

var deferred;
HermesInternal.enablePromiseRejectionTracker({
  allRejections: true,
  onUnhandled: function(id, error, p) {
    print("Unhandled:", error, "samePromise:", p === deferred);
  },
});

var rejectedPromise = new Promise(function(res, rej) {
  rej("failure!");
});

// `.then()` attaches a handler to `rejectedPromise`, which fires `_B` and
// clears its rejection entry. The rejection then propagates to the deferred
// promise that `.then()` returns; that's the promise the tracker eventually
// reports as unhandled.
deferred = rejectedPromise.then(function() {
  print('resolved');
});
// CHECK-NEXT: Unhandled: failure! samePromise: true

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s

// The promise rejection tracker must not keep a rejected promise alive after
// its timer fires, and a tracker must ignore the rejections of an earlier
// enablePromiseRejectionTracker() call.

print('promise rejection tracking');
// CHECK-LABEL: promise rejection tracking

function enableTracker(name, allRejections) {
  HermesInternal.enablePromiseRejectionTracker({
    allRejections: allRejections,
    onUnhandled: function (id, reason) {
      print(name, 'unhandled', id, reason.message);
    },
    onHandled: function (id, reason) {
      print(name, 'handled', id, reason.message);
    },
  });
}

// Rejects a promise with no handler, and returns a WeakRef to the reason.
function rejectUnhandled(message) {
  var reason = new Error(message);
  Promise.reject(reason);
  return new WeakRef(reason);
}

// Runs fn in a later task, after the timers of the tracker.
function later(fn) {
  setTimeout(fn, 2500);
}

function checkCollected(name, ref) {
  // A WeakRef keeps its target alive until the end of the current job, so
  // collect in a new task.
  later(function () {
    gc();
    print(name, 'collected:', ref.deref() === undefined);
  });
}

// A reported rejection.
enableTracker('A', true);
var reported = rejectUnhandled('reported');
later(function () {
  checkCollected('reported', reported);
  // CHECK-NEXT: A unhandled 0 reported
  // CHECK-NEXT: reported collected: true

  // A rejection that the whitelist does not report.
  later(function () {
    enableTracker('B', false);
    var filtered = rejectUnhandled('filtered');
    later(function () {
      checkCollected('filtered', filtered);
      // CHECK-NEXT: filtered collected: true

      // Enable a new tracker while a rejection of the old one is pending.
      later(function () {
        enableTracker('C', true);
        var p = Promise.reject(new Error('p'));
        enableTracker('D', true);
        Promise.reject(new Error('q'));
        p.catch(function () {});
        later(function () {
          print('end');
        });
      });
    });
  });
});
// CHECK-NEXT: D unhandled 0 q
// CHECK-NEXT: end

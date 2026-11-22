/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xes6-promise %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xmicrotask-queue %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -emit-binary -out %t.hbc %s && %hermes -Xes6-promise %t.hbc | %FileCheck --match-full-lines %s

print('Promise.withResolvers');
// CHECK-LABEL: Promise.withResolvers

function PromiseLike(executor) {
  executor(
      function (v) { print('Resolved:', v) },
      function (e) { print('Rejected:', e) }
  );
}

var {resolve} = Promise.withResolvers.call(PromiseLike)
resolve('message1')
// CHECK-NEXT: Resolved: message1

var {resolve, promise} = Promise.withResolvers();
resolve('success withResolver!');
promise.then(function (message) {
  print('Resolved:', message);
});
// CHECK-NEXT: Resolved: success withResolver!

var {reject, promise} = Promise.withResolvers();
reject('failure withResolver!');
promise.then(function (message) {
  print('Resolved:', message);
}).catch(function(e) {
  print('Rejection:', e)
});
// CHECK-NEXT: Rejection: failure withResolver!

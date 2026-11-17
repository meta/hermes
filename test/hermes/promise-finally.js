/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xes6-promise %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xmicrotask-queue %s | %FileCheck --match-full-lines %s
// RUN: %hermesc -O -emit-binary -out %t.hbc %s && %hermes -Xes6-promise %t.hbc | %FileCheck --match-full-lines %s

// Regression test for Promise.prototype.finally with a non-callable
// onFinally. Per ECMA-262 §27.2.5.3, when IsCallable(onFinally) is false
// the value/rejection of the receiver must pass straight through.

print('promise-finally');
// CHECK-LABEL: promise-finally

// 1) No-arg call: value passes through.
Promise.resolve('ok').finally().then(function (v) {
  print('no-arg:', v);
});
// CHECK-NEXT: no-arg: ok

// 2) Explicit undefined: value passes through.
Promise.resolve('ok').finally(undefined).then(function (v) {
  print('undefined:', v);
});
// CHECK-NEXT: undefined: ok

// 3) null: value passes through.
Promise.resolve('ok').finally(null).then(function (v) {
  print('null:', v);
});
// CHECK-NEXT: null: ok

// 4) Rejection passes through unchanged when onFinally is not callable.
Promise.reject(new Error('x')).finally().then(function () {
  print('rejection: unexpected resolve');
}, function (e) {
  print('rejection:', e.message);
});
// CHECK-NEXT: rejection: x

// 5) Sanity: callable onFinally still works (no regression).
Promise.resolve('ok').finally(function () {}).then(function (v) {
  print('callable:', v);
});
// CHECK-NEXT: callable: ok

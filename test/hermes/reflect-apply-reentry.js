/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

// Re-entry through native frames only: every level is Reflect.apply bound to
// a native iteration method, whose callback is the next level. The handles
// that Reflect.apply allocates must not pile up in the shared GCScope (Debug
// builds assert on too many handles in one scope).

print('reflect apply reentry');
// CHECK-LABEL: reflect apply reentry

var calls = 0;
var chain = function () {
  calls++;
};
for (var i = 0; i < 100; i++)
  chain = Reflect.apply.bind(null, Array.prototype.forEach, [1], [chain]);
chain();
print('Array.prototype.forEach: ' + calls);
// CHECK-NEXT: Array.prototype.forEach: 1

calls = 0;
chain = function () {
  calls++;
};
for (var i = 0; i < 100; i++)
  chain = Reflect.apply.bind(
    null,
    Map.prototype.forEach,
    new Map([[1, 1]]),
    [chain],
  );
chain();
print('Map.prototype.forEach: ' + calls);
// CHECK-NEXT: Map.prototype.forEach: 1

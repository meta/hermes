/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -lazy %s | %FileCheck --match-full-lines %s

// Async arrow functions whose rest parameter is a destructuring pattern.

print('async arrow rest pattern');
// CHECK-LABEL: async arrow rest pattern

(async () => {
  var arrayRest = async (...[a, b = 'default']) => a + ' ' + b;
  print(await arrayRest('x'));
// CHECK-NEXT: x default

  var leadingParam = async (x, ...[y, z]) => x + y + z;
  print(await leadingParam(1, 2, 3));
// CHECK-NEXT: 6

  var objectRest = async (...{length}) => length;
  print(await objectRest(1, 2, 3));
// CHECK-NEXT: 3

  var nestedRest = async (...[[a], {b}]) => a + b;
  print(await nestedRest([1], {b: 2}));
// CHECK-NEXT: 3

  function boom() {
    throw new Error('boom');
  }
  var throwingDefault = async (...[a = boom()]) => a;
  try {
    await throwingDefault();
  } catch (e) {
    print('caught', e.message);
  }
// CHECK-NEXT: caught boom
})();

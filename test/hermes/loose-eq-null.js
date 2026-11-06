/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O0 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// test/jit/loose-eq-null.js runs this file under the JIT.

// Loose equality against null and undefined, in value and branch position,
// for a value of every type.

function eqNull(x) {
  return x == null;
}
function neqNull(x) {
  return x != null;
}
function eqUndefined(x) {
  return undefined == x;
}
function neqUndefined(x) {
  return undefined != x;
}
function branchEq(x) {
  if (x == null)
    return 'T';
  return 'F';
}
function branchNeq(x) {
  if (x != null)
    return 'T';
  return 'F';
}

var values = [
  undefined,
  null,
  0,
  -0,
  NaN,
  1.5,
  '',
  'null',
  false,
  true,
  Symbol(),
  10n,
  {},
  [],
  function () {},
];
var out = '';
for (var v of values) {
  out +=
    (eqNull(v) ? 'T' : 'F') +
    (neqNull(v) ? 'T' : 'F') +
    (eqUndefined(v) ? 'T' : 'F') +
    (neqUndefined(v) ? 'T' : 'F') +
    branchEq(v) +
    branchNeq(v) +
    ' ';
}
print(out);
// CHECK: TFTFTF TFTFTF FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT FTFTFT

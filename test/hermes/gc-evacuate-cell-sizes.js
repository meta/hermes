/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -gc-sanitize-handles=0 %s | %FileCheck --match-full-lines %s

// A young-gen collection copies each live cell into the old gen in a different
// way depending on the size of the cell. Strings of lengths up to 200 give
// cells of every size from the smallest string up to past the largest that is
// copied inline. With HV32, numbers that do not fit inline, such as n / 3, are
// boxed in the smallest cells. The arrays and objects give cells of many sizes
// that also hold pointers. Check that all of them survive the copy.

var pattern = '';
for (var i = 0; i < 256; ++i)
  pattern += String.fromCharCode(32 + ((i * 37) % 95));

function makeString(n) {
  return pattern.substring(n % 13, (n % 13) + n);
}

var cells = [];
for (var n = 0; n < 200; ++n) {
  var arr = [];
  for (var i = 0; i < n; ++i)
    arr.push(n * 1000 + i);
  var obj = {};
  for (var i = 0; i < n % 32; ++i)
    obj['p' + i] = n * 1000 + i;
  cells.push(makeString(n), n / 3, arr, obj);
}

gc();

var errors = 0;
function check(cond, what, n) {
  if (!cond) {
    print('Bad ' + what + ' ' + n);
    ++errors;
  }
}
for (var n = 0; n < 200; ++n) {
  check(cells[4 * n] === makeString(n), 'string', n);
  check(cells[4 * n + 1] === n / 3, 'number', n);
  var arr = cells[4 * n + 2];
  check(arr.length === n, 'array length', n);
  for (var i = 0; i < n; ++i)
    check(arr[i] === n * 1000 + i, 'array element', n);
  var obj = cells[4 * n + 3];
  check(Object.keys(obj).length === n % 32, 'object size', n);
  for (var i = 0; i < n % 32; ++i)
    check(obj['p' + i] === n * 1000 + i, 'object property', n);
}
print(errors === 0 ? 'OK' : 'FAILED');
// CHECK: OK

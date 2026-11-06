/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --match-full-lines %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// After a four-entry chain (own pointer, own boxed double, prototype
// string, prototype boxed double) is installed, late own shapes hit the
// live-cache generic tier inline; primitives go to the helper. 1.1, 2.2
// and 3.3 are not representable inline under HV32/BOXED, so they
// exercise the boxed decode.
//
// Version 2 declines exactly 6 times (first read of each late shape,
// four primitive reads), below the threshold: NumRecompileChecks stays
// at version 1's single crossing. If late reads missed the generic
// tier they would decline 600 times. main()'s own ById reads: four
// `.prototype`, `.tag` once -- below the threshold.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(arr[i % arr.length]);
    return r;
  }
  function Pr() {}
  Pr.prototype.v = 'proto';
  function Pd() { this.k = 0; }
  Pd.prototype.v = 2.2;
  var o1 = {v: {tag: 'obj'}};
  var o2 = {w: 0, v: 1.1};
  var o3 = new Pr();
  var o4 = new Pd();
  drive(get, [o1, o2, o3, o4], 300);
  print(drive(get, [{z: 0, y: 0, v: 3.3}], 300));
  print(drive(get, [{u: 0, v: 'late'}], 300));
  print(get(o1).tag, get(o2), get(o3), get(o4));
  print(get('str'), get(7));
  String.prototype.v = 'sv';
  Number.prototype.v = 42;
  print(get('str'), get(7));
}
main();

// CHECK: 3.3
// CHECK-NEXT: late
// CHECK-NEXT: obj 1.1 proto 2.2
// CHECK-NEXT: undefined undefined
// CHECK-NEXT: sv 42
// CHECK: NumRecompileChecks: 1
// CHECK: NumRecompiles: 1

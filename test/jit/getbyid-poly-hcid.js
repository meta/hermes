/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-hc-id-limit=1 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit

// The lazy ID space is exhausted. One ID exists. Version 2 emits one
// entry and marks the other two unemittable, so no further progress:
// exactly one recompile, and the site then demotes on the
// stable-crossing schedule.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 3]);
    return s;
  }
  print(drive(get, [{x: 1}, {a: 0, x: 2}, {b: 0, x: 3}], 3000));
}
main();

// CHECK: 6000
// CHECK: NumRecompiles: 1{{$}}
// CHECK: NumByIdDemotions: 1{{$}}

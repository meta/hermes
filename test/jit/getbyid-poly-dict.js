/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit

// An own entry on a cacheable dictionary receiver class is emitted in
// the chain (the DUMP check: two entries, d's and plain's); a later
// delete gives the receiver a new class and is read correctly.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    for (var i = 0; i < n; ++i)
      f(arr[i & 1]);
  }
  var d = {x: 'dict', t: 0};
  delete d.t;  // cacheable dictionary
  var plain = {y: 0, x: 'plain'};
  drive(get, [d, plain], 200);
  print(get(d), get(plain));
  delete d.x;
  print(get(d));
  d.x = 'again';
  print(get(d));
}
main();

// CHECK: dict plain
// CHECK-NEXT: undefined
// CHECK-NEXT: again
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (2 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)

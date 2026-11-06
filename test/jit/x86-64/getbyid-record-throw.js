/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// A recording site whose forwarded lookup throws, 200 times: the
// exception reaches the caller every time, and the throwing calls are
// neither recorded nor counted -- so no threshold crossing happens at
// all. Counting before forwarding would produce 200 / 16 = 12 checks.
// The throw longjmps past the helper, so its LocalsRAII must not exist
// yet when the forwarded call runs; the gc() below catches a stale one.
function main() {
  var proto = {
    get boom() {
      throw new Error('boom');
    },
  };
  function get(o) {
    return o.boom;
  }
  function drive(f, o, n) {
    var caught = 0;
    for (var i = 0; i < n; ++i) {
      try {
        f(o);
      } catch (e) {
        ++caught;
      }
    }
    return caught;
  }
  var caught = drive(get, Object.create(proto), 200);
  // The helper's rooted locals must be gone after each throw: a stale
  // Locals registration would be walked by this collection.
  gc();
  print(caught);
}
main();

// CHECK: 200
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 0{{$}}
// CHECK: NumByIdDemotions: 0{{$}}

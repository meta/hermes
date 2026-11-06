/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=2 %s | %FileCheck %s
// REQUIRES: jit

// A getter reached through the recording helper retires `get`'s
// running version, then makes the outer call's receiver cacheable and
// fills the shared cache for it, so the outer helper call -- returning
// into the retired version -- finds a recordable shape. Only the
// staleness gate keeps it out of the retired record.
//
// Inside the getter: (1) 200 polymorphic reads cross the threshold,
// recompile `get` and retire version 1 (whose helper call is still on
// the stack); (2) an own data property `v` gives `tricky` a new class;
// (3) get(this) fills the shared cache with that own shape. Back in
// version 1's helper, the cache now describes the receiver's current
// class. With the gate, nothing is recorded into the retired record;
// without it, the Debug invariant in markWeakElementsInCaches fires at
// the next collection (gc() at the latest). `marker` keeps tricky's
// class unique.
//
// -gc-sanitize-handles=1 makes a HERMESVM_SANITIZE_HANDLES build move
// every live object at every allocation (its default rate is 1%); other
// builds ignore the flag.
function main() {
  function get(o) {
    return o.v;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i & 1]);
    return s;
  }
  var shapes = [{v: 1}, {a: 0, v: 2}];
  var reentered = false;
  var proto = {
    get v() {
      if (!reentered) {
        reentered = true;
        drive(get, shapes, 200);
        Object.defineProperty(this, 'v', {
          value: 7,
          writable: true,
          enumerable: true,
          configurable: true,
        });
        get(this);
      }
      return 100;
    },
  };
  var tricky = Object.create(proto);
  tricky.marker = 1;
  var s = get(tricky) + drive(get, shapes, 10) + get(tricky);
  gc();
  print(s);
}
main();

// 100 (outer getter call) + 15 + 7 (now an own data property).
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: 122

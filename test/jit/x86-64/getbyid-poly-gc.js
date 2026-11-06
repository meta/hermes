/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -gc-sanitize-handles=1 -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck %s
// REQUIRES: jit

// A recorded, never-emitted prototype entry whose prototype class dies
// while the receiver class lives. The record's weak root (and the read
// cache's) are cleared by gc(); the dead entry is then overwritten by
// the prototype's new shape, so version 2 emits exactly one entry:
// the dead one is neither emitted nor misread as an own entry. Without
// the gc(), version 2 emits both prototype entries.
//
// The receiver keeps its class throughout. A receiver's hidden class
// fixes its prototype, so swapping the prototype with setPrototypeOf
// would change the receiver class too; instead the prototype object
// itself moves to a new class. Adding enough properties puts it in
// dictionary mode, whose class has no transition parent, so the class
// recorded in phase 1 is left unreferenced.
//
// -gc-sanitize-handles=1 makes a HERMESVM_SANITIZE_HANDLES build move
// every live object at every allocation (its default rate is 1%); other
// builds ignore the flag.
function main() {
  function get(o) {
    return o.uniqueGcProp;
  }
  function drive(f, o, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(o);
    return r;
  }
  // A prototype whose class nothing else uses; 8 reads (< 16): recorded,
  // no crossing, never emitted, so its class is not pinned.
  var p = {uniqueGcPropA: 0, uniqueGcProp: 'p1'};
  var r = Object.create(p);
  drive(get, r, 8);
  for (var i = 0; i < 100; ++i)
    p['filler' + i] = i;
  p.uniqueGcProp = 'p2';
  gc();
  print(drive(get, r, 100));
}
main();

// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: // Get polymorphic chain (1 entries)
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// CHECK: p2

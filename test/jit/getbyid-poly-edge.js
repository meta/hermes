/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Chain edge cases: out-of-line (propStorage) slots for own and
// prototype entries, prototype entries whose receivers are empty
// objects, an empty receiver with no parent, and a Proxy receiver.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var r;
    for (var i = 0; i < n; ++i)
      r = f(arr[i & 3]);
    return r;
  }
  // Out-of-line slots: 12 properties before `x` (more than the direct
  // slots, fewer than the dictionary threshold).
  function mk(prefix, last) {
    var o = {};
    for (var i = 0; i < 12; ++i)
      o[prefix + i] = i;
    o.x = last;
    return o;
  }
  // Prototype entries whose receivers are empty objects.
  function E1() {}
  E1.prototype.x = 'e1';
  function E2() {}
  E2.prototype.x = 'e2';
  var big = mk('p', 'bigown');
  var viaBig = Object.create(mk('q', 'bigproto'));
  var shapes = [new E1(), new E2(), big, viaBig];
  drive(get, shapes, 400);
  print(get(shapes[0]), get(shapes[1]), get(big), get(viaBig));
  // An empty receiver with no parent: the prototype block's null exit.
  print(get(Object.create(null)));
  print(get(new Proxy({}, {get: function () { return 'trap'; }})));
}
main();

// Four entries, one per shape: E1.prototype and E2.prototype acquire the
// same properties in the same order and share a class, but a receiver's
// hidden class fixes its prototype, so the empty receivers new E1(),
// new E2() and Object.create(...) have three different classes, and each
// is its own (receiver, prototype class) entry next to big's own entry.
// CHECK: e1 e2 bigown bigproto
// CHECK-NEXT: undefined
// CHECK-NEXT: trap
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)

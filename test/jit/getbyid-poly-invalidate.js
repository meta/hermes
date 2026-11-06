/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// REQUIRES: jit

// After a chain with own and prototype entries is installed, each
// mutation below is observed through chain misses, never through a
// stale inline load. The DUMP check pins the premise: version 2's chain
// holds p's and q's prototype entries and own's own entry.
function main() {
  function get(o) {
    return o.m;
  }
  function drive(f, arr, n) {
    for (var i = 0; i < n; ++i)
      f(arr[i % 3]);
  }
  function P() {}
  P.prototype.m = 'proto';
  function Q() { this.k = 0; }
  Q.prototype.m = 'qproto';
  var p = new P(), q = new Q(), own = {m: 'own'};
  drive(get, [p, q, own], 300);
  print(get(p), get(q), get(own));
  // 1. Shadowing: p gains an own `m` (new receiver class).
  p.m = 'shadow';
  print(get(p));
  // 2. Prototype swap: setPrototypeOf moves q2 to a new receiver class,
  // which no chain entry names.
  var q2 = new Q();
  Object.setPrototypeOf(q2, {m: 'swapped'});
  print(get(q2));
  // 3. The prototype becomes a dictionary through delete.
  delete Q.prototype.m;
  print(get(new Q()));
  // 4. A data property redefined as a getter on the prototype.
  Object.defineProperty(P.prototype, 'm', {
    get: function () { return 'getter'; },
  });
  print(get(new P()));
  drive(get, [new P(), new Q(), own], 300);
  print(get(new P()), get(new Q()), get(own));
}
main();

// CHECK: proto qproto own
// CHECK-NEXT: shadow
// CHECK-NEXT: swapped
// CHECK-NEXT: undefined
// CHECK-NEXT: getter
// CHECK-NEXT: getter undefined own
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
